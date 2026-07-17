#include "VirtualFileSystem.h"
#include "ArchiveReader.h"
#include "StringUtil.h"
#include "FloarLog.h"
#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace Floar
{
namespace
{
    bool StartsWith(const std::wstring& str, const std::wstring& prefix)
    {
        if (str.length() < prefix.length())
        {
            return false;
        }
        return str.compare(0, prefix.length(), prefix) == 0;
    }

    // Archive-reader cache. Opening a reader parses the whole file table
    // (Ba2Reader/BsaReader::ScanArchive), so re-Opening the same archive for
    // every ListDirectory/ReadFile/IsDirectory call - e.g. once per thumbnail
    // while browsing a BSA/BA2 - is O(entries) wasted work per call and starves
    // the UI + load pool. A scanned reader is read-only (ExtractToMemory opens
    // its own local file handle each call; ListEntries/HasEntry only read const
    // tables), so one cached reader is safe to share across threads. Keyed by
    // lower-cased path + last-write-time so an archive rebuilt on disk re-scans.
    std::mutex g_readerMutex;
    struct CachedReader
    {
        std::shared_ptr<IArchiveReader> reader;
        std::filesystem::file_time_type mtime {};
    };
    std::unordered_map<std::wstring, CachedReader> g_readerCache;
    constexpr size_t kMaxCachedReaders = 16; // browsing touches only a few at once

    std::shared_ptr<IArchiveReader> GetCachedReader(const std::filesystem::path& archivePath)
    {
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(archivePath, ec);
        const std::wstring key = StringUtil::ToLower(archivePath.wstring());

        std::lock_guard<std::mutex> lk(g_readerMutex);
        if (auto it = g_readerCache.find(key); it != g_readerCache.end())
        {
            // Reuse unless the file changed on disk. If the mtime is unavailable
            // (ec), trust the cached reader for the session.
            if (ec || it->second.mtime == mtime)
            {
                return it->second.reader;
            }
        }

        std::shared_ptr<IArchiveReader> reader = ArchiveReaderFactory::Open(archivePath);
        if (reader)
        {
            // Bound the cache: readers hold only file-table metadata (not file
            // contents), but don't let a long browsing session grow unbounded.
            if (g_readerCache.size() >= kMaxCachedReaders && g_readerCache.find(key) == g_readerCache.end())
            {
                g_readerCache.clear();
            }
            g_readerCache[key] = CachedReader{ reader, ec ? std::filesystem::file_time_type{} : mtime };
        }
        return reader;
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

bool VirtualFileSystem::IsDirectory(const VirtualPath& path)
{
    if (path.IsInArchive())
    {
        auto reader = GetCachedReader(path.hostPath);
        if (!reader)
        {
            return false;
        }

        std::wstring normalized = NormalizePath(path.archiveInnerPath);
        std::wstring searchPrefix = normalized;
        if (!searchPrefix.empty() && searchPrefix.back() != L'/')
        {
            searchPrefix += L'/';
        }

        auto entries = reader->ListEntries();
        for (const auto& entry : entries)
        {
            std::wstring entryNormalized = NormalizePath(entry.name);
            if (entryNormalized == normalized && entry.isDirectory)
            {
                return true;
            }
            if (StartsWith(entryNormalized, searchPrefix))
            {
                return true;
            }
        }
        return false;
    }
    else if (path.IsArchiveFile())
    {
        return true;
    }
    else
    {
        return std::filesystem::is_directory(path.hostPath);
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

    auto archiveEntries = reader->ListEntries();
    std::unordered_set<std::wstring> addedDirs;

    for (const auto& archEntry : archiveEntries)
    {
        std::wstring normalized = NormalizePath(archEntry.name);
        auto sepPos = normalized.find(L'/');
        if (sepPos == std::wstring::npos)
        {
            VirtualPath vpath(archivePath, normalized);
            entries.emplace_back(vpath, archEntry.isDirectory, archEntry.size, archEntry.modTime);
        }
        else
        {
            std::wstring dirName = normalized.substr(0, sepPos);
            if (addedDirs.insert(dirName).second)
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
    if (!searchPrefix.empty() && searchPrefix.back() != L'/')
    {
        searchPrefix += L'/';
    }
    const std::wstring searchPrefixLower = StringUtil::ToLower(searchPrefix);

    auto archiveEntries = reader->ListEntries();
    std::unordered_set<std::wstring> addedDirs;

    for (const auto& archEntry : archiveEntries)
    {
        std::wstring normalized = NormalizePath(archEntry.name);
        const std::wstring normalizedLower = StringUtil::ToLower(normalized);
        if (!StartsWith(normalizedLower, searchPrefixLower))
        {
            continue;
        }

        std::wstring relativePath = normalized.substr(searchPrefix.length());
        auto sepPos = relativePath.find(L'/');
        if (sepPos == std::wstring::npos)
        {
            VirtualPath vpath(archivePath, normalized);
            entries.emplace_back(vpath, archEntry.isDirectory, archEntry.size, archEntry.modTime);
        }
        else
        {
            std::wstring dirName = relativePath.substr(0, sepPos);
            std::wstring fullDirPath = searchPrefix + dirName;
            if (addedDirs.insert(fullDirPath).second)
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
    std::wstring result = path;
    std::replace(result.begin(), result.end(), L'\\', L'/');
    while (!result.empty() && result.back() == L'/')
    {
        result.pop_back();
    }
    return result;
}
}
