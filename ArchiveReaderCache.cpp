#include "ArchiveReaderCache.h"
#include "VirtualPath.h"

#include <algorithm>
#include <system_error>

#include <Windows.h>

namespace Floar
{
ArchiveReaderCache::ArchiveReaderCache(size_t capacity, Opener opener, size_t maxOpenWorkers)
    : m_capacity(capacity == 0 ? 1 : capacity)
    , m_opener(opener ? std::move(opener) : Opener([](const std::filesystem::path& path, std::stop_token stop)
        {
            return std::shared_ptr<IArchiveReader>(ArchiveReaderFactory::Open(path, stop));
        }))
    , m_maxWorkers(maxOpenWorkers == 0 ? 1 : maxOpenWorkers)
{
}

ArchiveReaderCache::~ArchiveReaderCache()
{
    // Last resort for an owner that did not call Shutdown: never detach.
    while (!Shutdown(std::chrono::hours(1)))
    {
    }
}

ArchiveReaderCache& ArchiveReaderCache::Shared()
{
    static ArchiveReaderCache cache;
    return cache;
}

std::filesystem::path ArchiveReaderCache::Resolve(const std::filesystem::path& archivePath)
{
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(archivePath, ec);
    return ec ? archivePath : absolute;
}

std::wstring ArchiveReaderCache::Key(const std::filesystem::path& archivePath)
{
    return HostPathKey(Resolve(archivePath));
}

ArchiveReaderCache::Identity ArchiveReaderCache::ReadIdentity(const std::filesystem::path& archivePath)
{
    // One attribute query: directory_entry caches size and write time.
    Identity identity {};
    std::error_code ec;
    const std::filesystem::directory_entry entry(archivePath, ec);
    if (ec)
    {
        return identity;
    }
    const std::uintmax_t size = entry.file_size(ec);
    if (ec)
    {
        return identity;
    }
    const std::filesystem::file_time_type writeTime = entry.last_write_time(ec);
    if (ec)
    {
        return identity;
    }
    identity.known = true;
    identity.size = size;
    identity.writeTime = writeTime;
    return identity;
}

std::shared_ptr<IArchiveReader> ArchiveReaderCache::Get(const std::filesystem::path& archivePath, std::stop_token stop)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown)
        {
            return nullptr;
        }
    }

    // One absolute path for the key, the change check and the open.
    const std::filesystem::path path = Resolve(archivePath);
    const std::wstring key = HostPathKey(path);
    const Identity identity = ReadIdentity(path);

    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_shutdown)
    {
        return nullptr;
    }

    std::shared_ptr<Slot> slot;
    if (auto it = m_nodes.find(key); it != m_nodes.end())
    {
        m_lru.splice(m_lru.begin(), m_lru, it->second.lruPosition);
        slot = it->second.slot;
    }
    else
    {
        slot = std::make_shared<Slot>();
        m_lru.push_front(key);
        m_nodes.emplace(key, Node { slot, m_lru.begin() });
    }
    ++slot->activeLookups;   // pins the slot against eviction
    EvictLocked();

    std::shared_ptr<IArchiveReader> reader;
    if (slot->reader && (!identity.known || slot->identity == identity))
    {
        reader = slot->reader;
    }
    else if (!stop.stop_requested())
    {
        // Join the archive's queued or running open, or queue a new one.
        std::shared_ptr<Operation> operation = slot->current;
        if (!operation)
        {
            operation = std::make_shared<Operation>();
            operation->path = path;
            operation->identity = identity;
            operation->slot = slot;
            operation->queued = true;
            slot->current = operation;
            m_queue.push_back(operation);
            StartWorkerIfNeededLocked();
            m_workAvailable.notify_one();
        }

        ++operation->interest;
        ++m_waiting;
        // Our token only ends our own wait; the open has its own stop_source.
        (void)operation->finished.wait(lock, stop, [&] { return operation->done; });
        --m_waiting;
        --operation->interest;
        if (operation->done)
        {
            reader = operation->reader;
        }
        else if (operation->interest == 0)
        {
            RetireLocked(operation);   // nobody else wants it
        }
    }

    --slot->activeLookups;
    // Do not let unopenable paths occupy capacity.
    if (!slot->reader && !slot->current && slot->activeLookups == 0)
    {
        if (auto it = m_nodes.find(key); it != m_nodes.end() && it->second.slot == slot)
        {
            m_lru.erase(it->second.lruPosition);
            m_nodes.erase(it);
        }
    }
    EvictLocked();   // entries skipped while pinned
    return reader;
}

void ArchiveReaderCache::RetireLocked(const std::shared_ptr<Operation>& operation)
{
    if (operation->retired)
    {
        return;
    }
    operation->retired = true;
    operation->done = true;
    operation->reader.reset();
    operation->stop.request_stop();
    if (operation->queued)
    {
        operation->queued = false;
        std::erase(m_queue, operation);
    }
    if (operation->worker != nullptr && operation->worker->handle != nullptr)
    {
        // Best effort, and only while that worker runs this operation (both
        // are changed under m_mutex), so it never reaches the worker's next open.
        (void)CancelSynchronousIo(static_cast<HANDLE>(operation->worker->handle));
    }
    // A later caller starts a replacement operation instead of joining this one.
    if (operation->slot->current == operation)
    {
        operation->slot->current.reset();
    }
    operation->finished.notify_all();
}

void ArchiveReaderCache::StartWorkerIfNeededLocked()
{
    if (m_shutdown || m_queue.size() <= m_idleWorkers || m_workers.size() >= m_maxWorkers)
    {
        return;
    }
    Worker& worker = m_workers.emplace_back();
    try
    {
        // The worker waits for the lock held here before touching `worker`.
        worker.thread = std::thread(&ArchiveReaderCache::WorkerMain, this, &worker);
    }
    catch (...)
    {
        m_workers.pop_back();
        if (m_workers.empty())
        {
            // Nothing could ever run the queue: release its waiters.
            while (!m_queue.empty())
            {
                RetireLocked(std::shared_ptr<Operation>(m_queue.front()));
            }
        }
        return;
    }
    worker.handle = worker.thread.native_handle();
    ++m_liveWorkers;
}

void ArchiveReaderCache::WorkerMain(Worker* self)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;)
    {
        ++m_idleWorkers;
        m_workAvailable.wait(lock, [&] { return m_shutdown || !m_queue.empty(); });
        --m_idleWorkers;
        if (m_shutdown)
        {
            break;
        }

        std::shared_ptr<Operation> operation = std::move(m_queue.front());
        m_queue.pop_front();
        operation->queued = false;
        operation->worker = self;
        self->operation = operation;
        ++m_running;
        ++m_openCount;
        const std::filesystem::path path = operation->path;
        const std::stop_token stop = operation->stop.get_token();

        lock.unlock();
        std::shared_ptr<IArchiveReader> reader;
        try
        {
            reader = m_opener(path, stop);
        }
        catch (...)
        {
            reader.reset();
        }
        lock.lock();

        // Out of the opener: no OS cancel targets this worker from here on.
        operation->worker = nullptr;
        self->operation.reset();
        --m_running;
        if (operation->retired)
        {
            // Every caller left (or Shutdown): publish nothing, not even a
            // complete reader, and free it outside the lock.
            lock.unlock();
            reader.reset();
            operation.reset();
            lock.lock();
            continue;
        }

        operation->done = true;
        operation->reader = reader;
        Slot& slot = *operation->slot;
        slot.reader = reader;
        slot.identity = operation->identity;
        if (slot.current == operation)
        {
            slot.current.reset();
        }
        operation->finished.notify_all();
    }

    --m_liveWorkers;
    m_workerExited.notify_all();
}

bool ArchiveReaderCache::Shutdown(std::chrono::milliseconds bound)
{
    std::list<Worker> workers;
    std::unordered_map<std::wstring, Node> nodes;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_shutdown)
        {
            m_shutdown = true;
            while (!m_queue.empty())
            {
                RetireLocked(std::shared_ptr<Operation>(m_queue.front()));
            }
            for (Worker& worker : m_workers)
            {
                if (worker.operation)
                {
                    RetireLocked(std::shared_ptr<Operation>(worker.operation));
                }
            }
            m_workAvailable.notify_all();
        }

        if (!m_workerExited.wait_for(lock, bound, [&] { return m_liveWorkers == 0; }))
        {
            return false;   // still owned: a later call waits again
        }
        workers = std::move(m_workers);
        m_workers.clear();
        // Waiters may still be leaving Get; their slots stay alive through their
        // own references. Release the readers outside the lock.
        nodes = std::move(m_nodes);
        m_nodes.clear();
        m_lru.clear();
    }

    // Every worker has left its loop; join them before any static teardown.
    for (Worker& worker : workers)
    {
        if (worker.thread.joinable())
        {
            worker.thread.join();
        }
    }
    return true;
}

void ArchiveReaderCache::EvictLocked()
{
    auto it = m_lru.end();
    while (m_nodes.size() > m_capacity && it != m_lru.begin())
    {
        --it;
        auto node = m_nodes.find(*it);
        if (node->second.slot->activeLookups != 0)
        {
            continue;   // in use: keep its coordination state
        }
        m_nodes.erase(node);
        it = m_lru.erase(it);
    }
}

size_t ArchiveReaderCache::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nodes.size();
}

size_t ArchiveReaderCache::OpenCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_openCount;
}

size_t ArchiveReaderCache::WorkerCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_workers.size();
}

size_t ArchiveReaderCache::RunningOpenCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_running;
}

size_t ArchiveReaderCache::QueuedOpenCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

size_t ArchiveReaderCache::WaitingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_waiting;
}
}
