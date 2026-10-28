#pragma once

#include "VirtualPath.h"
#include <cstddef>
#include <functional>
#include <stop_token>
#include <vector>
#include <span>

namespace Floar
{
class ArchiveReaderCache;

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
    // Plain files are read in chunks of this size, with a stop check between them.
    static constexpr size_t kPlainFileReadChunk = 2 * 1024 * 1024;

    static std::vector<VirtualFileEntry> ListDirectory(const VirtualPath& dirPath);

    // Streams the children of `dirPath` (a folder, an archive, or a folder in
    // an archive) to `onEntry`. `stop` is checked before every file-system
    // operation, before an archive is opened, while waiting for the open (the
    // scan itself stops only once no other caller waits for it; a stopped scan
    // publishes no reader) and between entries. Returns false once stopped;
    // true when the listing completed, including folders that do not exist or
    // archives that cannot be opened (no entries). Runs on the calling thread,
    // except the archive open (on an ArchiveReaderCache worker): `onEntry` is
    // never called after the call returns and no cache lock is held while it
    // runs.
    static bool EnumerateDirectory(
        const VirtualPath& dirPath,
        std::stop_token stop,
        const std::function<void(VirtualFileEntry&&)>& onEntry);
    // The whole file, or empty when it cannot be read. Uses ArchiveReaderCache::Shared().
    // A plain file whose read fails or ends before the size queried when it
    // was opened (it shrank, an OS read error, ERROR_OPERATION_ABORTED after a
    // CancelSynchronousIo) returns empty data, never a partly filled buffer;
    // so does an empty file or one larger than 1 GiB.
    static std::vector<uint8_t> ReadFile(const VirtualPath& filePath);
    // Stop-aware read: empty once `stop` is requested, and nothing partial is
    // returned. `stop` is checked before any work (no open, extraction or file
    // open starts once it is requested), governs this caller's admission and
    // wait for the archive's reader (ArchiveReaderCache::Get: the shared open
    // stops only when no other caller waits for it), and is checked by the
    // extraction at entry and read-chunk boundaries and before the data is
    // returned (ZIP/7z/RAR also before every 64 KiB block read from the
    // archive, so libarchive's decompression of the entries it skips in a solid
    // archive ends within one block; BA2/BSA before and after the entry).
    // Cancellation is cooperative, not a wall-clock guarantee: one
    // decompression step or one OS read can block for its whole duration, and
    // a read blocked on a network path or an online-only file is not
    // interrupted by a stop check. Extraction and plain-file reads
    // run on the calling thread (its owner may add CancelSynchronousIo); the
    // archive open runs on a cache worker. Plain files: stop is checked before
    // the file is opened, then the file is read into one buffer in
    // kPlainFileReadChunk chunks with a stop check before each chunk and
    // before the data is returned, so a stop returns empty data after at most
    // one more chunk. One chunk's OS read can still block for its whole
    // duration (a network path, an online-only OneDrive file) and is not
    // interrupted by a stop check; only the caller's CancelSynchronousIo on
    // this thread can end it (the read then fails and returns empty data).
    static std::vector<uint8_t> ReadFile(const VirtualPath& filePath, std::stop_token stop);
    // The same, with archive readers from `cache` (an injected opener in tests).
    static std::vector<uint8_t> ReadFile(const VirtualPath& filePath, std::stop_token stop, ArchiveReaderCache& cache);
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
