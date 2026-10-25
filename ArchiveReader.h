#pragma once

#include <filesystem>
#include <memory>
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
    // File-system status queries made by IsArchiveFile, and Open calls (for tests).
    static size_t StatusQueryCountForTesting();
    static size_t OpenCountForTesting();
    static std::unique_ptr<IArchiveReader> Open(const std::filesystem::path& path);
    static bool HasArchiveExtension(const std::wstring& filename);
    static std::vector<std::wstring> GetSupportedExtensions();
};
}
