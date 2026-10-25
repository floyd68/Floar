#include "ArchiveReaderCache.h"
#include "VirtualPath.h"

#include <system_error>

namespace Floar
{
ArchiveReaderCache::ArchiveReaderCache(size_t capacity, Opener opener)
    : m_capacity(capacity == 0 ? 1 : capacity)
    , m_opener(opener ? std::move(opener) : Opener([](const std::filesystem::path& path, std::stop_token stop)
        {
            return std::shared_ptr<IArchiveReader>(ArchiveReaderFactory::Open(path, stop));
        }))
{
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
    // One absolute path for the key, the change check and the open.
    const std::filesystem::path path = Resolve(archivePath);
    const std::wstring key = HostPathKey(path);
    const Identity identity = ReadIdentity(path);

    std::shared_ptr<Slot> slot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
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
    }

    std::shared_ptr<IArchiveReader> reader;
    {
        // Only callers of this archive wait here.
        std::lock_guard<std::mutex> openLock(slot->openMutex);
        if (slot->reader && (!identity.known || slot->identity == identity))
        {
            reader = slot->reader;
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_openCount;
            }
            reader = m_opener(path, stop);
            slot->reader = reader;
            slot->identity = identity;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        --slot->activeLookups;
        // Do not let unopenable paths occupy capacity (unless a waiter retries).
        if (!reader && slot->activeLookups == 0)
        {
            if (auto it = m_nodes.find(key); it != m_nodes.end() && it->second.slot == slot)
            {
                m_lru.erase(it->second.lruPosition);
                m_nodes.erase(it);
            }
        }
        EvictLocked();   // entries skipped while pinned
    }
    return reader;
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
}
