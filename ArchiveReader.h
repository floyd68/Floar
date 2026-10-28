#pragma once

#include "ArchiveCursorPool.h"

#include <cstdint>
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
    // scan, delivering in archive order (7z/RAR on a forward cursor, see
    // ExtractForward; Normal priority); random-access formats (BA2/BSA)
    // extract each entry directly, in request order.
    //
    // Cancellation: `stop` is checked at every entry boundary (sequential
    // formats also between read chunks of an entry and before every block read
    // from the archive file; random-access formats before and after each
    // entry); once requested, no further entry is read or delivered, a partly
    // read entry is dropped, and the call returns false (true when every path
    // was delivered). Cooperative only: one decompression step or OS read is
    // not interrupted. Ownership: everything happens on the
    // calling thread and `onEntry` is never called after the call returns, so a
    // caller can drop a canceled generation without keeping reader state. No
    // cache lock is held while `onEntry` runs.
    virtual bool ExtractBatch(const std::vector<std::wstring>& entryPaths, std::stop_token stop, const BatchSink& onEntry);

    // Options of ExtractForward.
    struct ExtractOptions
    {
        // Admission priority for a cursor (ArchiveCursorPool): High for a
        // request the user waits on (a main image), Normal for background work.
        ExtractPriority priority { ExtractPriority::Normal };
    };

    // Called once per delivered entry; returning false ends the pass after it.
    using ForwardSink = std::function<bool(const std::wstring& entryPath, std::vector<uint8_t>&& data)>;

    enum class ExtractResult
    {
        Completed,    // every requested path was delivered
        EndedBySink,  // the sink returned false; later entries were not read or delivered
        Stopped,      // `stop` was requested; nothing partial was delivered
    };

    // ExtractBatch with an early end and a priority, for callers that batch
    // the entries they will need soon: delivery order, the empty data for
    // missing/directory/unreadable entries (delivered after the archive
    // entries), cancellation and the callback guarantees are those of
    // ExtractBatch. When `onEntry` returns false the pass ends at that entry
    // boundary: nothing more is read or delivered, and a sequential reader
    // keeps its cursor after that entry for the next request.
    //
    // Readers with HasExpensiveRandomAccess() (7z, RAR) run the pass on a
    // forward cursor from their ArchiveCursorPool: a request whose first
    // entry (in archive order) is at or after a retained cursor of this reader
    // continues it without reopening the archive or decompressing the entries
    // before it again; an earlier one restarts from the start of the archive.
    // Admission may wait for pool capacity (on `stop`). `onEntry` runs on the
    // calling thread while this call owns the cursor, never under a pool or
    // cache lock; it must not wait for another thread's extraction from the
    // same pool (a nested extraction on the same thread is admitted at once).
    // Other readers: one scan per call (ZIP) or each entry read directly in
    // request order (BA2/BSA), as ExtractBatch.
    virtual ExtractResult ExtractForward(
        const std::vector<std::wstring>& entryPaths,
        std::stop_token stop,
        const ForwardSink& onEntry,
        const ExtractOptions& options);

    // True when reading an entry costs decompressing the entries before it
    // (solid 7z, RAR): requests are cheaper batched in archive order. False
    // for ZIP, BA2 and BSA.
    virtual bool HasExpensiveRandomAccess() const { return false; }

    static constexpr size_t kNoArchiveOrder = static_cast<size_t>(-1);
    // Position of the entry in archive (extraction) order, for sorting a
    // batch; kNoArchiveOrder when missing, a directory, or the reader has no
    // meaningful order (BA2/BSA).
    virtual size_t ArchiveOrder(const std::wstring& entryPath) const;

    // Closes this reader's idle cursors and retains none from now on (called
    // when the archive was replaced or the reader was evicted from a cache).
    // Cursors in use are closed when their extraction ends.
    virtual void ReleaseIdleCursors() {}

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
    // first: other paths cost no file-system query. A path over MAX_PATH that
    // is not found is queried again in its extended-length form, as the
    // archive open retries it (a process that is not long-path aware).
    static bool IsArchiveFile(const std::filesystem::path& path);
    // Opens and scans `path`; null when it is not a readable archive or `stop`
    // was requested before the scan finished (a partial reader is never returned).
    static std::unique_ptr<IArchiveReader> Open(const std::filesystem::path& path, std::stop_token stop);
    // The same, with the cursors of sequential readers in `cursorPool`
    // (ArchiveCursorPool::Shared() for the other overloads).
    static std::unique_ptr<IArchiveReader> Open(
        const std::filesystem::path& path, std::stop_token stop, std::shared_ptr<ArchiveCursorPool> cursorPool);
    // File-system status queries made by IsArchiveFile, and Open calls (for tests).
    static size_t StatusQueryCountForTesting();
    static size_t OpenCountForTesting();
    // Archive file handles opened by readers to read entry data (for tests).
    static size_t DataHandleOpenCountForTesting();
    // Entry headers read by libarchive batch extractions (for tests).
    static size_t ExtractionHeaderCountForTesting();
    // Archive bytes libarchive readers read from their files (for tests).
    static size_t LibArchiveBytesReadForTesting();
    // Test seam, process-wide: called before every block a libarchive reader
    // reads from its archive file, with the file offset; true fails that read
    // (as an I/O error). Install only while no other libarchive read runs;
    // an empty function removes it.
    static void SetLibArchiveReadHookForTesting(std::function<bool(uint64_t offset)> hook);
    static std::unique_ptr<IArchiveReader> Open(const std::filesystem::path& path);
    static bool HasArchiveExtension(const std::wstring& filename);
    static std::vector<std::wstring> GetSupportedExtensions();
};
}
