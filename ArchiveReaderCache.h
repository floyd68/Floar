#pragma once

#include "ArchiveReader.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <unordered_map>

namespace Floar
{
// Shared archive readers, so browsing an archive scans its table once instead
// of on every ListDirectory/ReadFile/Exists/IsDirectory call. A scanned reader is
// read-only (ExtractToMemory opens its own file handle per call), so one cached
// reader is shared across threads.
//
// - Keys: HostPathKey of the absolute path (VirtualPath.h), so equivalent
//   spellings share one reader. The path is made absolute once per lookup and
//   that same path is used for the key, the change check and the open, so a
//   reader never keeps a relative path whose meaning changes with the current
//   directory.
// - Change detection: the file's size and last-write time, read once per
//   lookup. A reader is reopened when either differs. When the metadata cannot
//   be read (file locked, deleted, offline), a cached reader is kept - reads
//   then fail on their own if the file is gone - and with no cached reader the
//   archive is opened (which fails if it is missing).
// - Concurrency: the cache mutex only guards the table; opening and scanning
//   run under a per-archive lock, so one slow open never blocks hits on or
//   opens of other archives. Callers asking for the same archive while it opens
//   wait for that single open.
// - Capacity: least-recently-used readers are evicted one at a time; callers
//   still holding an evicted reader keep using it. An archive with a lookup in
//   progress (opening, or waiting for its open) is never evicted, so its
//   per-archive coordination survives and it is never opened twice at once;
//   the cache may exceed its capacity until those lookups finish.
class ArchiveReaderCache
{
public:
    // Opens and scans an archive; returns null when `stop` interrupted it.
    using Opener = std::function<std::shared_ptr<IArchiveReader>(const std::filesystem::path&, std::stop_token)>;

    static constexpr size_t kDefaultCapacity = 16;

    explicit ArchiveReaderCache(size_t capacity = kDefaultCapacity, Opener opener = {});

    // The cache behind VirtualFileSystem (ArchiveReaderFactory::Open).
    static ArchiveReaderCache& Shared();

    // The reader for `archivePath`, or null when it cannot be opened (not
    // cached). A cached reader is returned even when `stop` is requested; an
    // open interrupted by `stop` returns null and caches nothing, and callers
    // waiting for that open then open the archive themselves.
    std::shared_ptr<IArchiveReader> Get(const std::filesystem::path& archivePath, std::stop_token stop = {});

    size_t Size() const;
    // Number of opener calls so far.
    size_t OpenCount() const;

    static std::wstring Key(const std::filesystem::path& archivePath);

private:
    struct Identity
    {
        bool known { false };
        std::uintmax_t size { 0 };
        std::filesystem::file_time_type writeTime {};

        bool operator==(const Identity&) const = default;
    };

    struct Slot
    {
        std::mutex openMutex {};
        std::shared_ptr<IArchiveReader> reader {};  // guarded by openMutex
        Identity identity {};                       // guarded by openMutex
        size_t activeLookups { 0 };                 // guarded by m_mutex
    };

    struct Node
    {
        std::shared_ptr<Slot> slot {};
        std::list<std::wstring>::iterator lruPosition {};
    };

    static std::filesystem::path Resolve(const std::filesystem::path& archivePath);
    static Identity ReadIdentity(const std::filesystem::path& archivePath);
    // Evicts least-recently-used entries without an active lookup.
    void EvictLocked();

    const size_t m_capacity;
    const Opener m_opener;

    mutable std::mutex m_mutex {};
    std::list<std::wstring> m_lru {};  // most recently used first
    std::unordered_map<std::wstring, Node> m_nodes {};
    size_t m_openCount { 0 };
};
}
