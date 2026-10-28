#pragma once

#include "ArchiveReader.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Floar
{
// Shared archive readers, so browsing an archive scans its table once instead
// of on every ListDirectory/ReadFile/Exists/IsDirectory call. A scanned reader is
// read-only (ExtractToMemory opens its own file handle per call), so one cached
// reader is shared across threads.
//
// - Keys: HostPathKey of the absolute path (VirtualPath.h), so equivalent
//   spellings share one reader. The path is made absolute once per lookup and
//   that same path is used for the key, the change check and the open, so a
//   reader never keeps a relative path whose meaning changes with the current
//   directory.
// - Change detection: the file's size and last-write time, read once per
//   lookup. A reader is reopened when either differs. When the metadata cannot
//   be read (file locked, deleted, offline), a cached reader is kept - reads
//   then fail on their own if the file is gone - and with no cached reader the
//   archive is opened (which fails if it is missing).
// - Opens: every open and scan runs as one operation on a worker owned by the
//   cache (never on a caller's thread), at most `maxOpenWorkers` at once, one
//   operation per worker. Workers start lazily, when the first open needs one,
//   and live until Shutdown. Callers of an archive whose open is queued or
//   running for the same file identity register interest in that operation
//   instead of opening it again, and wait on their own `stop` token: a stopped
//   caller leaves at once without stopping the open. A caller that sees a
//   different identity (the file was replaced while it was being opened) gets
//   an operation of its own; the older one still serves its own callers, and
//   the newest finished generation is the one cached. The operation owns its
//   stop_source and is stopped only when its last interested caller left (it
//   is then retired: it publishes nothing, and a caller arriving afterwards
//   starts a replacement operation instead of joining the stopped one) or at
//   Shutdown.
// - Stopping: the stop request runs on a stop-dispatch thread owned by the
//   cache (started with the first stop of a running open), never under the
//   cache lock and never on a caller's thread, so the opener's stop callbacks
//   may query the cache, and one that blocks delays only that thread (Shutdown
//   still returns within its bound). After the request the dispatcher calls a
//   best-effort CancelSynchronousIo on the worker; that call is made under the
//   cache lock while the worker is still assigned to that operation, so it
//   never reaches the worker's next open. Not every OS call honours it.
// - Admission: one FIFO queue, one priority, shared by callers with and
//   without a stop token. A queued caller whose token stops leaves the queue
//   without taking a worker; one without a token waits until its open finishes
//   or Shutdown releases it. Limitation: there is no priority and no reserved
//   capacity, so an open of one archive waits behind the opens of other
//   archives queued before it, and while every worker is blocked in a slow open
//   (a large solid archive, a network path) every further open waits, including
//   untokened calls from a UI thread. Hits on cached readers never wait.
// - Capacity: least-recently-used readers are evicted one at a time; callers
//   still holding an evicted reader keep using it. An archive with a lookup in
//   progress (opening, or waiting for its open) is never evicted, so its
//   operation survives and it is never opened twice for one generation; the
//   cache may exceed its capacity until those lookups finish.
// - Cursors: a reader that is evicted, or replaced because its file changed,
//   has its idle forward cursors closed (IArchiveReader::ReleaseIdleCursors),
//   outside the cache lock; callers still holding it keep using it.
// - Lifetime: hosts and test executables call Shutdown(bound) before static
//   teardown (also for Shared()). The destructor joins the workers without a
//   bound and never detaches them.
class ArchiveReaderCache
{
public:
    // Opens and scans an archive; returns null when `stop` interrupted it. Runs
    // on a cache worker; `stop` is the operation's own token.
    using Opener = std::function<std::shared_ptr<IArchiveReader>(const std::filesystem::path&, std::stop_token)>;

    static constexpr size_t kDefaultCapacity = 16;
    static constexpr size_t kDefaultOpenWorkers = 2;

    // Starts no thread.
    explicit ArchiveReaderCache(size_t capacity = kDefaultCapacity, Opener opener = {}, size_t maxOpenWorkers = kDefaultOpenWorkers);
    ~ArchiveReaderCache();

    ArchiveReaderCache(const ArchiveReaderCache&) = delete;
    ArchiveReaderCache& operator=(const ArchiveReaderCache&) = delete;

    // The cache behind VirtualFileSystem (ArchiveReaderFactory::Open).
    static ArchiveReaderCache& Shared();

    // The reader for `archivePath`, or null when it cannot be opened (not
    // cached), when `stop` was requested before the reader was available, or
    // after Shutdown began. A cached reader is returned even when `stop` is
    // requested; with no cached reader a stopped caller starts no open. A
    // stopped caller never stops an open that other callers still wait for, and
    // those callers then get its reader without a rescan.
    std::shared_ptr<IArchiveReader> Get(const std::filesystem::path& archivePath, std::stop_token stop = {});

    // Stops every queued and running open (through its stop_source plus the
    // best-effort OS cancel), releases every waiting caller with null, and waits
    // up to `bound` for the workers and the stop dispatcher to finish - their
    // native threads included, so thread-local destructors have run. Returns
    // true once every thread was joined; false when one is still running (an
    // opener blocked in an OS call that ignores cancellation, a blocking stop
    // callback, a thread-local destructor), in which case the cache keeps
    // ownership of it and a later call waits again. Safe before first use (then
    // starts nothing and returns true) and repeatable. Once it began, Get returns
    // null and no worker or open starts.
    bool Shutdown(std::chrono::milliseconds bound);

    size_t Size() const;
    // Number of opener calls so far.
    size_t OpenCount() const;
    // Observability (tests): worker threads started and not yet joined, opens
    // running on a worker (retired ones included until their opener returns),
    // opens waiting for a worker, and callers waiting for an open.
    // ThreadCount adds the stop dispatcher (once started, until joined).
    size_t WorkerCount() const;
    size_t ThreadCount() const;
    size_t RunningOpenCount() const;
    size_t QueuedOpenCount() const;
    size_t WaitingCount() const;

    static std::wstring Key(const std::filesystem::path& archivePath);

private:
    struct Identity
    {
        bool known { false };
        std::uintmax_t size { 0 };
        std::filesystem::file_time_type writeTime {};

        bool operator==(const Identity&) const = default;
    };

    struct Operation;
    struct Worker;

    // Every field is guarded by m_mutex.
    struct Slot
    {
        std::shared_ptr<IArchiveReader> reader {};
        Identity identity {};
        std::vector<std::shared_ptr<Operation>> opens {};  // queued or running, not retired
        std::uint64_t nextGeneration { 0 };
        std::uint64_t publishedGeneration { 0 };            // of `reader`
        size_t activeLookups { 0 };
    };

    // One open of one archive generation. Every field is guarded by m_mutex.
    struct Operation
    {
        std::filesystem::path path {};
        Identity identity {};
        std::uint64_t generation { 0 };    // per slot, increasing
        std::shared_ptr<Slot> slot {};
        std::stop_source stop {};          // service-owned
        std::condition_variable_any finished {};
        size_t interest { 0 };             // callers waiting for `reader`
        bool queued { false };             // in m_queue
        Worker* worker { nullptr };        // running it, if any
        bool done { false };               // waiters are released with `reader`
        bool retired { false };            // stopped; publishes nothing
        std::shared_ptr<IArchiveReader> reader {};
    };

    struct Worker
    {
        std::thread thread {};
        void* handle { nullptr };            // native handle, for CancelSynchronousIo
        std::shared_ptr<Operation> operation {};
    };

    struct Node
    {
        std::shared_ptr<Slot> slot {};
        std::list<std::wstring>::iterator lruPosition {};
    };

    static std::filesystem::path Resolve(const std::filesystem::path& archivePath);
    static Identity ReadIdentity(const std::filesystem::path& archivePath);
    // Evicts least-recently-used entries without an active lookup, into `evicted`.
    void EvictLocked(std::vector<std::shared_ptr<Slot>>& evicted);
    // Closes the idle cursors of evicted readers (not under m_mutex).
    void ReleaseEvicted(std::vector<std::shared_ptr<Slot>>& evicted);
    // Stops `operation` for good and releases its waiters with null.
    void RetireLocked(const std::shared_ptr<Operation>& operation);
    // Starts a worker when queued opens exceed the idle workers and the bound allows it.
    void StartWorkerIfNeededLocked();
    void WorkerMain(Worker* self);
    // Starts the stop-dispatch thread on the first stop of a running open.
    void StartDispatcherIfNeededLocked();
    void DispatcherMain();

    const size_t m_capacity;
    const Opener m_opener;
    const size_t m_maxWorkers;

    mutable std::mutex m_mutex {};
    std::list<std::wstring> m_lru {};  // most recently used first
    std::unordered_map<std::wstring, Node> m_nodes {};
    size_t m_openCount { 0 };

    std::condition_variable m_workAvailable {};
    std::condition_variable m_workerExited {};
    std::deque<std::shared_ptr<Operation>> m_queue {};
    std::list<Worker> m_workers {};  // stable addresses; joined only by Shutdown
    size_t m_liveWorkers { 0 };
    size_t m_idleWorkers { 0 };
    size_t m_running { 0 };
    size_t m_waiting { 0 };
    bool m_shutdown { false };

    // Stop dispatch: running operations whose stop is still to be requested.
    std::condition_variable m_stopAvailable {};
    std::deque<std::shared_ptr<Operation>> m_stopQueue {};
    std::thread m_dispatcher {};
    void* m_dispatcherHandle { nullptr };
    bool m_dispatcherLive { false };  // started and not yet out of its loop
    bool m_dispatcherExit { false };

    // Serializes Shutdown calls (each waits for it within its own bound).
    std::timed_mutex m_shutdownMutex {};
};
}
