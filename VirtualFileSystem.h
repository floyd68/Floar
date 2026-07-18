#pragma once

#include "VirtualPath.h"
#include <functional>
#include <vector>
#include <span>

namespace Floar
{
struct VirtualFileEntry
{
    VirtualPath path;
    bool isDirectory;
    uint64_t size;
    time_t modTime;

    VirtualFileEntry() = default;

    VirtualFileEntry(const VirtualPath& p, bool isDir, uint64_t sz = 0, time_t mt = 0)
        : path(p)
        , isDirectory(isDir)
        , size(sz)
        , modTime(mt)
    {
    }
};

class VirtualFileSystem
{
public:
    static std::vector<VirtualFileEntry> ListDirectory(const VirtualPath& dirPath);
    static std::vector<uint8_t> ReadFile(const VirtualPath& filePath);
    static bool Exists(const VirtualPath& path);
    static bool IsDirectory(const VirtualPath& path);

    static void EnumerateFilesystemDirectory(
        const std::filesystem::path& dirPath,
        const std::function<void(VirtualFileEntry&&)>& onEntry);

private:
    static std::vector<VirtualFileEntry> ListFilesystemDirectory(const std::filesystem::path& dirPath);
    static std::vector<VirtualFileEntry> ListArchiveRoot(const std::filesystem::path& archivePath);
    static std::vector<VirtualFileEntry> ListArchiveSubdirectory(
        const std::filesystem::path& archivePath,
        const std::wstring& innerPath);
    static std::wstring NormalizePath(const std::wstring& path);
};
}
