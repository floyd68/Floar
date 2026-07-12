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

class IArchiveReader
{
public:
    virtual ~IArchiveReader() = default;

    virtual std::vector<ArchiveEntry> ListEntries() = 0;
    virtual std::vector<uint8_t> ExtractToMemory(const std::wstring& entryPath) = 0;
    virtual bool HasEntry(const std::wstring& entryPath) = 0;
    virtual std::wstring GetFormatName() const = 0;
};

class ArchiveReaderFactory
{
public:
    static bool IsArchiveFile(const std::filesystem::path& path);
    static std::unique_ptr<IArchiveReader> Open(const std::filesystem::path& path);
    static bool HasArchiveExtension(const std::wstring& filename);
    static std::vector<std::wstring> GetSupportedExtensions();
};
}
