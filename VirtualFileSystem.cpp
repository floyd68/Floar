#include "VirtualFileSystem.h"
#include "ArchiveReader.h"
#include "ArchiveReaderCache.h"
#include "StringUtil.h"
#include "FloarLog.h"
#include <algorithm>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_set>

#include <Windows.h>

namespace Floar
{
namespace
{
    std::shared_ptr<IArchiveReader> GetCachedReader(const std::filesystem::path& archivePath)
    {
        return ArchiveReaderCache::Shared().Get(archivePath);
    }

    constexpr int64_t kMaxPlainFileSize = 1024 * 1024 * 1024;

    std::mutex g_plainFileReadHookMutex;
    std::shared_ptr<const VirtualFileSystem::PlainFileReadHook> g_plainFileReadHook;

    std::shared_ptr<const VirtualFileSystem::PlainFileReadHook> CurrentPlainFileReadHook()
    {
        std::lock_guard lock(g_plainFileReadHookMutex);
        return g_plainFileReadHook;
    }

    struct FileHandle
    {
        HANDLE handle { INVALID_HANDLE_VALUE };
        ~FileHandle()
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
            }
        }
    };

    // The whole file, or empty: a file that cannot be opened, is empty or
    // larger than kMaxPlainFileSize, or whose read fails or ends before the
    // size queried at open (EOF, an OS error such as ERROR_OPERATION_ABORTED)
    // returns no data, never a partly filled buffer. One allocation; the data
    // is read in kPlainFileReadChunk chunks with a stop check before each one
    // and before returning, so a stop discards the data after at most one more
    // chunk read (which itself is not interrupted).
    std::vector<uint8_t> ReadPlainFile(const std::filesystem::path& path, std::stop_token stop)
    {
        const auto hook = CurrentPlainFileReadHook();
        if (hook && hook->beforeOpen)
        {
            hook->beforeOpen(path);
        }

        FileHandle file;
        file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file.handle == INVALID_HANDLE_VALUE)
        {
            return {};
        }

        LARGE_INTEGER size {};
        if (!GetFileSizeEx(file.handle, &size) || size.QuadPart <= 0 || size.QuadPart > kMaxPlainFileSize)
        {
            return {};
        }

        std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
        size_t offset = 0;
        while (offset < data.size())
        {
            if (stop.stop_requested())
            {
                return {};
            }
            const DWORD chunk = static_cast<DWORD>((std::min)(data.size() - offset, VirtualFileSystem::kPlainFileReadChunk));
            DWORD read = 0;
            DWORD error = ERROR_SUCCESS;
            if (hook && hook->beforeChunk)
            {
                error = hook->beforeChunk(offset, chunk);
            }
            if (error == ERROR_SUCCESS && !::ReadFile(file.handle, data.data() + offset, chunk, &read, nullptr))
            {
                error = GetLastError();
            }
            if (error == ERROR_OPERATION_ABORTED)
            {
                // The caller's CancelSynchronousIo.
                FLOAR_LOG_DEBUG("[VFS] Plain file read aborted at {} of {} bytes: {}", offset, data.size(), Log::ToUtf8(path));
                return {};
            }
            if (error != ERROR_SUCCESS)
            {
                FLOAR_LOG_WARN("[VFS] Plain file read failed at {} of {} bytes (error {}): {}",
                    offset, data.size(), error, Log::ToUtf8(path));
                return {};
            }
            offset += read;
            if (read != chunk)
            {
                FLOAR_LOG_WARN("[VFS] Plain file ended at {} of {} bytes: {}", offset, data.size(), Log::ToUtf8(path));
                return {};
            }
        }
        if (stop.stop_requested())
        {
            return {};
        }
        return data;
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

void VirtualFileSystem::SetPlainFileReadHook(PlainFileReadHook hook)
{
    std::shared_ptr<const PlainFileReadHook> installed;
    if (hook.beforeOpen || hook.beforeChunk)
    {
        installed = std::make_shared<const PlainFileReadHook>(std::move(hook));
    }
    std::lock_guard lock(g_plainFileReadHookMutex);
    g_plainFileReadHook = std::move(installed);
}

std::vector<uint8_t> VirtualFileSystem::ReadFile(const VirtualPath& filePath)
{
    return ReadFile(filePath, std::stop_token {}, ArchiveReaderCache::Shared());
}

std::vector<uint8_t> VirtualFileSystem::ReadFile(const VirtualPath& filePath, std::stop_token stop)
{
    return ReadFile(filePath, stop, ArchiveReaderCache::Shared());
}

std::vector<uint8_t> VirtualFileSystem::ReadFile(const VirtualPath& filePath, std::stop_token stop, ArchiveReaderCache& cache)
{
    return ReadFile(filePath, stop, cache, IArchiveReader::ExtractOptions {});
}

std::vector<uint8_t> VirtualFileSystem::ReadFile(
    const VirtualPath& filePath, std::stop_token stop, ArchiveReaderCache& cache, const IArchiveReader::ExtractOptions& options)
{
    if (stop.stop_requested())
    {
        return {};
    }
    if (filePath.IsInArchive())
    {
        auto reader = cache.Get(filePath.hostPath, stop);
        if (stop.stop_requested())
        {
            return {};
        }
        if (!reader)
        {
            FLOAR_LOG_WARN("[VFS] Failed to open archive reader.");
            return {};
        }

        std::vector<uint8_t> data;
        const auto result = reader->ExtractForward({ filePath.archiveInnerPath }, stop,
            [&data](const std::wstring&, std::vector<uint8_t>&& entryData)
            {
                data = std::move(entryData);
                return true;
            }, options);
        // Nothing partial or late: a stop during extraction drops the data.
        if (result != IArchiveReader::ExtractResult::Completed || stop.stop_requested())
        {
            return {};
        }
        if (data.empty())
        {
            FLOAR_LOG_WARN("[VFS] Archive entry read returned empty data.");
        }
        return data;
    }
    return ReadPlainFile(filePath.hostPath, stop);
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
