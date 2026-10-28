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
    std::vector<std::shared_ptr<Slot>> evicted;
    EvictLocked(evicted);

    std::shared_ptr<IArchiveReader> reader;
    if (slot->reader && (!identity.known || slot->identity == identity))
    {
        reader = slot->reader;
    }
    else if (!stop.stop_requested())
    {
        // Join the newest queued or running open of this file identity, or
        // queue a new one: a replaced file never gets an older generation.
        std::shared_ptr<Operation> operation;
        for (const auto& open : slot->opens)
        {
            if (!identity.known || open->identity == identity)
            {
                operation = open;
            }
        }
        if (!operation)
        {
            operation = std::make_shared<Operation>();
            operation->path = path;
            operation->identity = identity;
            operation->generation = ++slot->nextGeneration;
            operation->slot = slot;
            operation->queued = true;
            slot->opens.push_back(operation);
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
    if (!slot->reader && slot->opens.empty() && slot->activeLookups == 0)
    {
        if (auto it = m_nodes.find(key); it != m_nodes.end() && it->second.slot == slot)
        {
            m_lru.erase(it->second.lruPosition);
            m_nodes.erase(it);
        }
    }
    EvictLocked(evicted);   // entries skipped while pinned
    lock.unlock();
    ReleaseEvicted(evicted);
    return reader;
}

void ArchiveReaderCache::ReleaseEvicted(std::vector<std::shared_ptr<Slot>>& evicted)
{
    // New lookups get a new reader: an evicted one keeps serving its current
    // callers but retains no cursor (closed here, outside the cache lock).
    for (const auto& slot : evicted)
    {
        std::shared_ptr<IArchiveReader> reader;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            reader = slot->reader;
        }
        if (reader)
        {
            reader->ReleaseIdleCursors();
        }
    }
    evicted.clear();
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
    if (operation->queued)
    {
        // Never started: nobody holds its token, so there is nothing to stop.
        operation->queued = false;
        std::erase(m_queue, operation);
    }
    else if (operation->worker != nullptr)
    {
        // request_stop runs the opener's stop callbacks: never under m_mutex
        // and never on a caller's thread.
        m_stopQueue.push_back(operation);
        StartDispatcherIfNeededLocked();
        m_stopAvailable.notify_one();
    }
    // A later caller starts a replacement operation instead of joining this one.
    std::erase(operation->slot->opens, operation);
    operation->finished.notify_all();
}

void ArchiveReaderCache::StartDispatcherIfNeededLocked()
{
    if (m_dispatcher.joinable())
    {
        return;
    }
    try
    {
        // The dispatcher waits for the lock held here before reading the queue.
        m_dispatcher = std::thread(&ArchiveReaderCache::DispatcherMain, this);
    }
    catch (...)
    {
        // No thread: the stop stays queued for a later start. Like the OS
        // cancel this is best effort; Shutdown still reports a stuck open.
        return;
    }
    m_dispatcherHandle = m_dispatcher.native_handle();
    m_dispatcherLive = true;
}

void ArchiveReaderCache::DispatcherMain()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;)
    {
        m_stopAvailable.wait(lock, [&] { return m_dispatcherExit || !m_stopQueue.empty(); });
        if (m_stopQueue.empty())
        {
            break;   // exits only once every queued stop was dispatched
        }
        std::shared_ptr<Operation> operation = std::move(m_stopQueue.front());
        m_stopQueue.pop_front();

        lock.unlock();
        operation->stop.request_stop();   // may run (and block in) opener callbacks
        lock.lock();

        if (operation->worker != nullptr && operation->worker->handle != nullptr)
        {
            // Best effort, and only while that worker still runs this operation
            // (both are changed under m_mutex), so it never reaches the
            // worker's next open.
            (void)CancelSynchronousIo(static_cast<HANDLE>(operation->worker->handle));
        }
        lock.unlock();
        operation.reset();   // may release the last reference to its slot
        lock.lock();
    }

    m_dispatcherLive = false;
    m_workerExited.notify_all();
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

        if (reader && !operation->retired && operation->generation > operation->slot->publishedGeneration &&
            operation->slot->reader && operation->slot->reader != reader)
        {
            // A newer generation (the file changed) is about to replace the
            // published reader, whose cursors read the old content: close them
            // before any caller sees the new one, outside the lock (still
            // counted as running, so Shutdown retires it meanwhile). Callers
            // still holding the old reader keep using it, without cursors.
            std::shared_ptr<IArchiveReader> stale = operation->slot->reader;
            lock.unlock();
            stale->ReleaseIdleCursors();
            stale.reset();
            lock.lock();
        }
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
        // An older generation finishing late never replaces a newer one.
        if (operation->generation > slot.publishedGeneration)
        {
            slot.reader = reader;
            slot.identity = operation->identity;
            slot.publishedGeneration = operation->generation;
        }
        std::erase(slot.opens, operation);
        operation->finished.notify_all();
    }

    --m_liveWorkers;
    m_workerExited.notify_all();
}

bool ArchiveReaderCache::Shutdown(std::chrono::milliseconds bound)
{
    const auto deadline = std::chrono::steady_clock::now() + (std::min)(bound, std::chrono::milliseconds(std::chrono::hours(24 * 365)));
    const auto remainingMs = [&]() -> DWORD
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        return static_cast<DWORD>(std::clamp<long long>(left.count(), 0, INFINITE - 1));
    };

    // One Shutdown at a time; a concurrent one waits within its own bound.
    std::unique_lock<std::timed_mutex> shutdownLock(m_shutdownMutex, deadline);
    if (!shutdownLock.owns_lock())
    {
        return false;
    }

    std::vector<HANDLE> threads;
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
            m_dispatcherExit = true;
            m_stopAvailable.notify_all();
        }

        if (!m_workerExited.wait_until(lock, deadline, [&] { return m_liveWorkers == 0 && !m_dispatcherLive; }))
        {
            return false;   // still owned: a later call waits again
        }
        for (Worker& worker : m_workers)
        {
            threads.push_back(static_cast<HANDLE>(worker.handle));
        }
        if (m_dispatcher.joinable())
        {
            threads.push_back(static_cast<HANDLE>(m_dispatcherHandle));
        }
    }

    // Leaving the loop is not the end of the thread: thread-local destructors
    // still run on it. Join only threads whose native thread has ended.
    for (HANDLE thread : threads)
    {
        if (WaitForSingleObject(thread, remainingMs()) != WAIT_OBJECT_0)
        {
            return false;   // still owned: a later call waits again
        }
    }

    std::list<Worker> workers;
    std::thread dispatcher;
    std::unordered_map<std::wstring, Node> nodes;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        workers = std::move(m_workers);
        m_workers.clear();
        dispatcher = std::move(m_dispatcher);
        m_dispatcherHandle = nullptr;
        // Waiters may still be leaving Get; their slots stay alive through their
        // own references. Release the readers outside the lock.
        nodes = std::move(m_nodes);
        m_nodes.clear();
        m_lru.clear();
    }

    // Every thread has ended; join them before any static teardown.
    for (Worker& worker : workers)
    {
        if (worker.thread.joinable())
        {
            worker.thread.join();
        }
    }
    if (dispatcher.joinable())
    {
        dispatcher.join();
    }
    return true;
}

void ArchiveReaderCache::EvictLocked(std::vector<std::shared_ptr<Slot>>& evicted)
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
        evicted.push_back(node->second.slot);
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

size_t ArchiveReaderCache::ThreadCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_workers.size() + (m_dispatcher.joinable() ? 1 : 0);
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
