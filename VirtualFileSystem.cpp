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
    if (dirPath.IsInArchive())
    {
        return ListArchiveSubdirectory(dirPath.hostPath, dirPath.archiveInnerPath);
    }
    else if (dirPath.IsArchiveFile())
    {
        return ListArchiveRoot(dirPath.hostPath);
    }
    else
    {
        return ListFilesystemDirectory(dirPath.hostPath);
    }
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
    if (!onEntry || !std::filesystem::exists(dirPath) || !std::filesystem::is_directory(dirPath))
    {
        return;
    }

    std::error_code iterEc;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::directory_iterator it(dirPath, options, iterEc);
    if (iterEc)
    {
        return;
    }

    const std::filesystem::directory_iterator end {};
    for (; it != end; it.increment(iterEc))
    {
        if (iterEc)
        {
            iterEc.clear();
            continue;
        }

        const std::filesystem::directory_entry& entry = *it;
        std::error_code typeEc;
        const bool isDir = entry.is_directory(typeEc);
        if (typeEc)
        {
            continue;
        }
        onEntry(VirtualFileEntry(VirtualPath(entry.path()), isDir, 0, 0));
    }
}

std::vector<VirtualFileEntry> VirtualFileSystem::ListFilesystemDirectory(const std::filesystem::path& dirPath)
{
    std::vector<VirtualFileEntry> entries;
    EnumerateFilesystemDirectory(dirPath, [&entries](VirtualFileEntry&& entry)
    {
        entries.push_back(std::move(entry));
    });
    return entries;
}

std::vector<VirtualFileEntry> VirtualFileSystem::ListArchiveRoot(const std::filesystem::path& archivePath)
{
    std::vector<VirtualFileEntry> entries;
    auto reader = GetCachedReader(archivePath);
    if (!reader)
    {
        return entries;
    }

    std::unordered_set<std::wstring> addedDirs;   // ArchiveInnerPathKey
    for (const auto& archEntry : reader->Entries())
    {
        std::wstring normalized = NormalizePath(archEntry.name);
        auto sepPos = normalized.find(L'/');
        if (sepPos == std::wstring::npos)
        {
            if (archEntry.isDirectory && !addedDirs.insert(ArchiveInnerPathKey(normalized)).second)
            {
                continue;   // already listed as the parent of an entry
            }
            VirtualPath vpath(archivePath, normalized);
            entries.emplace_back(vpath, archEntry.isDirectory, archEntry.size, archEntry.modTime);
        }
        else
        {
            std::wstring dirName = normalized.substr(0, sepPos);
            if (addedDirs.insert(ArchiveInnerPathKey(dirName)).second)
            {
                VirtualPath vpath(archivePath, dirName);
                entries.emplace_back(vpath, true, 0, 0);
            }
        }
    }
    return entries;
}

std::vector<VirtualFileEntry> VirtualFileSystem::ListArchiveSubdirectory(
    const std::filesystem::path& archivePath,
    const std::wstring& innerPath)
{
    std::vector<VirtualFileEntry> entries;
    auto reader = GetCachedReader(archivePath);
    if (!reader)
    {
        return entries;
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
            VirtualPath vpath(archivePath, normalized);
            entries.emplace_back(vpath, archEntry.isDirectory, archEntry.size, archEntry.modTime);
        }
        else
        {
            std::wstring dirName = relativePath.substr(0, sepPos);
            std::wstring fullDirPath = searchPrefix + dirName;
            if (addedDirs.insert(ArchiveInnerPathKey(fullDirPath)).second)
            {
                VirtualPath vpath(archivePath, fullDirPath);
                entries.emplace_back(vpath, true, 0, 0);
            }
        }
    }
    return entries;
}

std::wstring VirtualFileSystem::NormalizePath(const std::wstring& path)
{
    return NormalizeArchiveInnerPath(path);
}
}
