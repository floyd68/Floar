#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>
#include <span>

namespace Floar
{
struct ArchiveEntry
{
    std::wstring name;
    uint64_t size;
    uint64_t compressedSize;
    bool isDirectory;
    time_t modTime;
};

// Entry and directory lookups use ArchiveInnerPathKey (VirtualPath.h): either
// separator, no leading/trailing '/', case-insensitive.
class IArchiveReader
{
public:
    virtual ~IArchiveReader() = default;

    // The entry table, scanned when the reader opens and immutable afterwards:
    // a cached reader is shared across threads and queried without copying it.
    virtual const std::vector<ArchiveEntry>& Entries() const = 0;
    // A copy of Entries().
    std::vector<ArchiveEntry> ListEntries() const { return Entries(); }

    virtual std::vector<uint8_t> ExtractToMemory(const std::wstring& entryPath) = 0;

    using BatchSink = std::function<void(const std::wstring& entryPath, std::vector<uint8_t>&& data)>;

    // Extracts several entries, calling `onEntry` once per requested path with
    // its data (empty when the entry is missing, a directory, or unreadable).
    // Sequential formats (ZIP/7z/RAR) service the whole batch in one archive
    // scan, delivering in archive order; random-access formats (BA2/BSA)
    // extract each entry directly, in request order.
    //
    // Cancellation: `stop` is checked at every entry boundary; once requested,
    // no further entry is read or delivered and the call returns false (true
    // when every path was delivered). Ownership: everything happens on the
    // calling thread and `onEntry` is never called after the call returns, so a
    // caller can drop a canceled generation without keeping reader state. No
    // cache lock is held while `onEntry` runs.
    virtual bool ExtractBatch(const std::vector<std::wstring>& entryPaths, std::stop_token stop, const BatchSink& onEntry);

    virtual bool HasEntry(const std::wstring& entryPath) = 0;
    // True when `dirPath` names a directory: an explicit directory entry or a
    // parent of any entry (formats may omit directory entries). O(1).
    virtual bool HasDirectory(const std::wstring& dirPath) const = 0;
    virtual std::wstring GetFormatName() const = 0;
};

class ArchiveReaderFactory
{
public:
    // A supported extension that names a regular file. The extension is checked
    // first: other paths cost no file-system query.
    static bool IsArchiveFile(const std::filesystem::path& path);
    // Opens and scans `path`; null when it is not a readable archive or `stop`
    // was requested before the scan finished (a partial reader is never returned).
    static std::unique_ptr<IArchiveReader> Open(const std::filesystem::path& path, std::stop_token stop);
    // File-system status queries made by IsArchiveFile, and Open calls (for tests).
    static size_t StatusQueryCountForTesting();
    static size_t OpenCountForTesting();
    // Archive file handles opened by readers to read entry data (for tests).
    static size_t DataHandleOpenCountForTesting();
    static std::unique_ptr<IArchiveReader> Open(const std::filesystem::path& path);
    static bool HasArchiveExtension(const std::wstring& filename);
    static std::vector<std::wstring> GetSupportedExtensions();
};
}
