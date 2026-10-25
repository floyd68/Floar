#include "ArchiveReaderCache.h"
#include "VirtualPath.h"

#include <system_error>

namespace Floar
{
ArchiveReaderCache::ArchiveReaderCache(size_t capacity, Opener opener)
    : m_capacity(capacity == 0 ? 1 : capacity)
    , m_opener(opener ? std::move(opener) : Opener([](const std::filesystem::path& path)
        {
            return std::shared_ptr<IArchiveReader>(ArchiveReaderFactory::Open(path));
        }))
{
}

ArchiveReaderCache& ArchiveReaderCache::Shared()
{
    static ArchiveReaderCache cache;
    return cache;
}

std::wstring ArchiveReaderCache::Key(const std::filesystem::path& archivePath)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(archivePath, ec);
    return HostPathKey(ec ? archivePath : absolute);
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

std::shared_ptr<IArchiveReader> ArchiveReaderCache::Get(const std::filesystem::path& archivePath)
{
    const std::wstring key = Key(archivePath);
    const Identity identity = ReadIdentity(archivePath);

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
            EvictLocked();
        }
    }

    std::shared_ptr<IArchiveReader> reader;
    {
        // Only callers of this archive wait here.
        std::lock_guard<std::mutex> openLock(slot->openMutex);
        if (slot->reader && (!identity.known || slot->identity == identity))
        {
            return slot->reader;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_openCount;
        }
        reader = m_opener(archivePath);
        slot->reader = reader;
        slot->identity = identity;
    }

    if (!reader)
    {
        // Do not let unopenable paths occupy capacity.
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_nodes.find(key); it != m_nodes.end() && it->second.slot == slot)
        {
            m_lru.erase(it->second.lruPosition);
            m_nodes.erase(it);
        }
    }
    return reader;
}

void ArchiveReaderCache::EvictLocked()
{
    while (m_nodes.size() > m_capacity && !m_lru.empty())
    {
        m_nodes.erase(m_lru.back());
        m_lru.pop_back();
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
