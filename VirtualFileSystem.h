#pragma once

#include "VirtualPath.h"
#include <functional>
#include <stop_token>
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

    // Streams the children of `dirPath` (a folder, an archive, or a folder in
    // an archive) to `onEntry`. `stop` is checked before every file-system
    // operation, before an archive is opened (a scan in progress also stops,
    // and its partial reader is discarded) and between entries. Returns false
    // once stopped; true when the listing completed, including folders that do
    // not exist or archives that cannot be opened (no entries). Runs entirely on
    // the calling thread: `onEntry` is never called after the call returns and
    // no cache lock is held while it runs.
    static bool EnumerateDirectory(
        const VirtualPath& dirPath,
        std::stop_token stop,
        const std::function<void(VirtualFileEntry&&)>& onEntry);
    static std::vector<uint8_t> ReadFile(const VirtualPath& filePath);
    static bool Exists(const VirtualPath& path);
    static bool IsDirectory(const VirtualPath& path);

    static void EnumerateFilesystemDirectory(
        const std::filesystem::path& dirPath,
        const std::function<void(VirtualFileEntry&&)>& onEntry);

private:
    static bool EnumerateFilesystem(
        const std::filesystem::path& dirPath,
        std::stop_token stop,
        const std::function<void(VirtualFileEntry&&)>& onEntry);
    static bool EnumerateArchive(
        const std::filesystem::path& archivePath,
        const std::wstring& innerPath,
        std::stop_token stop,
        const std::function<void(VirtualFileEntry&&)>& onEntry);
    static std::wstring NormalizePath(const std::wstring& path);
};
}
