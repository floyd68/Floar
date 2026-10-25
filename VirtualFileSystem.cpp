#include "VirtualFileSystem.h"
#include "ArchiveReader.h"
#include "ArchiveReaderCache.h"
#include "StringUtil.h"
#include "FloarLog.h"
#include <algorithm>
#include <fstream>
#include <memory>
#include <system_error>
#include <unordered_set>

namespace Floar
{
namespace
{
    std::shared_ptr<IArchiveReader> GetCachedReader(const std::filesystem::path& archivePath)
    {
        return ArchiveReaderCache::Shared().Get(archivePath);
    }
}

std::vector<VirtualFileEntry> VirtualFileSystem::ListDirectory(const VirtualPath& dirPath)
{
    std::vector<VirtualFileEntry> entries;
    (void)EnumerateDirectory(dirPath, std::stop_token {}, [&entries](VirtualFileEntry&& entry)
    {
        entries.push_back(std::move(entry));
    });
    return entries;
}

bool VirtualFileSystem::EnumerateDirectory(
    const VirtualPath& dirPath,
    std::stop_token stop,
    const std::function<void(VirtualFileEntry&&)>& onEntry)
{
    if (stop.stop_requested())
    {
        return false;
    }
    if (dirPath.IsInArchive())
    {
        return EnumerateArchive(dirPath.hostPath, dirPath.archiveInnerPath, stop, onEntry);
    }
    if (dirPath.IsArchiveFile())
    {
        return EnumerateArchive(dirPath.hostPath, std::wstring(), stop, onEntry);
    }
    return EnumerateFilesystem(dirPath.hostPath, stop, onEntry);
}

std::vector<uint8_t> VirtualFileSystem::ReadFile(const VirtualPath& filePath)
{
    if (filePath.IsInArchive())
    {
        auto reader = GetCachedReader(filePath.hostPath);
        if (!reader)
        {
            FLOAR_LOG_WARN("[VFS] Failed to open archive reader.");
            return {};
        }

        auto data = reader->ExtractToMemory(filePath.archiveInnerPath);
        if (data.empty())
        {
            FLOAR_LOG_WARN("[VFS] Archive entry read returned empty data.");
        }
        return data;
    }
    else
    {
        std::ifstream file(filePath.hostPath, std::ios::binary | std::ios::ate);
        if (!file)
        {
            return {};
        }

        auto size = file.tellg();
        if (size <= 0 || size > 1024 * 1024 * 1024)
        {
            return {};
        }

        std::vector<uint8_t> data(static_cast<size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(data.data()), size);
        return data;
    }
}

bool VirtualFileSystem::Exists(const VirtualPath& path)
{
    if (path.IsInArchive())
    {
        auto reader = GetCachedReader(path.hostPath);
        if (!reader)
        {
            return false;
        }
        if (reader->HasEntry(path.archiveInnerPath))
        {
            return true;
        }

        // Some archive formats omit explicit directory entries. Treat a path
        // as existing when at least one entry has it as a parent directory.
        return IsDirectory(path);
    }

    std::error_code ec;
    return std::filesystem::exists(path.hostPath, ec) && !ec;
}

bool VirtualFileSystem::IsDirectory(const VirtualPath& path)
{
    if (path.IsInArchive())
    {
        auto reader = GetCachedReader(path.hostPath);
        // Directory index lookup: no scan or copy of the entry table.
        return reader && reader->HasDirectory(path.archiveInnerPath);
    }
    else if (path.IsArchiveFile())
    {
        return true;
    }
    else
    {
        std::error_code ec;
        return std::filesystem::is_directory(path.hostPath, ec) && !ec;
    }
}

void VirtualFileSystem::EnumerateFilesystemDirectory(
    const std::filesystem::path& dirPath,
    const std::function<void(VirtualFileEntry&&)>& onEntry)
{
    if (onEntry)
    {
        (void)EnumerateFilesystem(dirPath, std::stop_token {}, onEntry);
    }
}

bool VirtualFileSystem::EnumerateFilesystem(
    const std::filesystem::path& dirPath,
    std::stop_token stop,
    const std::function<void(VirtualFileEntry&&)>& onEntry)
{
    // Every operation below may block (network paths): none starts once stop
    // is requested.
    std::error_code ec;
    const bool isDirectory = std::filesystem::is_directory(dirPath, ec);
    if (stop.stop_requested())
    {
        return false;
    }
    if (ec || !isDirectory)
    {
        return true;
    }

    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::directory_iterator it(dirPath, options, ec);
    if (stop.stop_requested())
    {
        return false;
    }
    if (ec)
    {
        return true;
    }

    const std::filesystem::directory_iterator end {};
    bool skipCurrent = false;   // the last advance failed
    while (it != end)
    {
        if (stop.stop_requested())
        {
            return false;
        }
        if (!skipCurrent)
        {
            const std::filesystem::directory_entry& entry = *it;
            std::error_code typeEc;
            const bool isDir = entry.is_directory(typeEc);
            if (!typeEc)
            {
                onEntry(VirtualFileEntry(VirtualPath(entry.path()), isDir, 0, 0));
            }
        }
        if (stop.stop_requested())
        {
            return false;
        }
        it.increment(ec);
        skipCurrent = static_cast<bool>(ec);
        ec.clear();
    }
    return true;
}

bool VirtualFileSystem::EnumerateArchive(
    const std::filesystem::path& archivePath,
    const std::wstring& innerPath,
    std::stop_token stop,
    const std::function<void(VirtualFileEntry&&)>& onEntry)
{
    // A canceled open returns no reader (and caches none).
    auto reader = ArchiveReaderCache::Shared().Get(archivePath, stop);
    if (stop.stop_requested())
    {
        return false;
    }
    if (!reader)
    {
        return true;
    }

    std::wstring searchPrefix = NormalizePath(innerPath);
    if (!searchPrefix.empty())
    {
        searchPrefix += L'/';
    }
    const std::wstring searchPrefixKey = StringUtil::ToLower(searchPrefix);

    std::unordered_set<std::wstring> addedDirs;   // ArchiveInnerPathKey
    for (const auto& archEntry : reader->Entries())
    {
        if (stop.stop_requested())
        {
            return false;
        }
        std::wstring normalized = NormalizePath(archEntry.name);
        if (normalized.length() <= searchPrefix.length() ||
            StringUtil::ToLower(normalized.substr(0, searchPrefix.length())) != searchPrefixKey)
        {
            continue;   // outside this directory, or the directory entry itself
        }

        std::wstring relativePath = normalized.substr(searchPrefix.length());
        auto sepPos = relativePath.find(L'/');
        if (sepPos == std::wstring::npos)
        {
            if (archEntry.isDirectory && !addedDirs.insert(ArchiveInnerPathKey(normalized)).second)
            {
                continue;   // already listed as the parent of an entry
            }
            onEntry(VirtualFileEntry(VirtualPath(archivePath, normalized), archEntry.isDirectory, archEntry.size, archEntry.modTime));
        }
        else
        {
            std::wstring fullDirPath = searchPrefix + relativePath.substr(0, sepPos);
            if (addedDirs.insert(ArchiveInnerPathKey(fullDirPath)).second)
            {
                onEntry(VirtualFileEntry(VirtualPath(archivePath, fullDirPath), true, 0, 0));
            }
        }
    }
    // The last callback may have requested stop.
    return !stop.stop_requested();
}

std::wstring VirtualFileSystem::NormalizePath(const std::wstring& path)
{
    return NormalizeArchiveInnerPath(path);
}
}
