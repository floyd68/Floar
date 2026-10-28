#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace Floar
{
// Who is waiting for an extraction. High is for a request the user waits on
// directly (a main image); Normal for background work (thumbnails).
enum class ExtractPriority
{
    Normal,
    High,
};

// Bounds of an ArchiveCursorPool. Counts are cursors; memory is the sum of the
// readers' per-cursor estimates (decompressor dictionary and buffers).
struct ArchiveCursorLimits
{
    // Cursors in use by an extraction at once, process-wide and per reader.
    size_t maxActive { 3 };
    size_t maxActivePerReader { 2 };
    // Of each active limit, the part only High requests may use, so a main
    // image never waits behind Normal (thumbnail) cursors; Normal requests may
    // use the limit minus this, but at least one.
    size_t reservedForHigh { 1 };
    // Retained cursors (positioned after the last entry they read, holding
    // their decompressor state), process-wide and per reader.
    size_t maxIdle { 2 };
    size_t maxIdlePerReader { 2 };
    // Estimated memory of active and idle cursors. Idle cursors are closed to
    // make room; a Normal request that still does not fit waits, unless no
    // cursor is active (then it is admitted alone, so one oversized cursor can
    // always run). High requests are bounded by their count only.
    std::uint64_t maxMemoryBytes { 256ull * 1024 * 1024 };
    // An idle cursor older than this is closed at the next pool operation
    // (there is no timer thread; TrimIdle closes every idle cursor at once).
    // An idle cursor holds no handle to its archive file (it reopens the file
    // when it resumes and is refused if the file changed), so the archive can
    // be renamed over, rewritten or deleted while cursors are retained.
    std::chrono::milliseconds idleTimeout { std::chrono::seconds(15) };
};

// Counters since the pool was created, and the current state.
struct ArchiveCursorStats
{
    size_t active { 0 };
    size_t idle { 0 };
    size_t waiting { 0 };
    std::uint64_t memoryBytes { 0 };   // estimated, active + idle
    size_t peakActive { 0 };
    size_t peakIdle { 0 };
    std::uint64_t peakMemoryBytes { 0 };

    std::uint64_t opens { 0 };         // admissions that open the archive and read from its start
    std::uint64_t restarts { 0 };      // opens while the reader had idle cursors, all past the target
    std::uint64_t resumes { 0 };       // admissions that continue a retained cursor
    std::uint64_t retained { 0 };      // cursors returned to idle at a valid boundary
    std::uint64_t discarded { 0 };     // cursors closed after use (error, stop inside the archive, end reached, exception)
    std::uint64_t evicted { 0 };       // idle cursors closed by limits, timeout, TrimIdle or ReleaseOwner
    std::uint64_t invalidated { 0 };   // cursors refused because the archive file changed
    std::uint64_t waits { 0 };         // admissions that had to wait for capacity
    std::uint64_t canceledWaits { 0 }; // waits ended by stop (no capacity kept)
    std::uint64_t headersRead { 0 };   // entry headers advanced on cursors, skipped ones included
    std::uint64_t entriesRead { 0 };   // requested entries read on cursors
};

// Bounded set of forward cursors over sequential archives (solid 7z, RAR), so
// successive extractions in archive order continue where the previous one
// stopped instead of reopening the archive and decompressing from its start.
//
// - A cursor is exclusive: it belongs to one Lease (one extraction call) or
//   sits idle in the pool, never both, and is never shared or closed while in
//   use. Its position is the number of entry headers consumed; it is reusable
//   for any request whose first entry is at or after that position.
// - Acquire returns a lease: the best retained cursor of that reader (the
//   furthest one not past the target), or capacity to open a new one. At
//   capacity it waits in FIFO order (High before Normal) on the caller's stop
//   token; a stopped waiter leaves without taking or leaking capacity.
// - The pool lock is never held while reading the archive, opening or closing
//   a cursor, or running a caller's callback: closing happens on the thread
//   that released or evicted the cursor, after the lock is dropped.
// - A thread that already holds a lease (a nested extraction from inside an
//   entry callback) is admitted at once - within the limits when they allow
//   it, beyond them otherwise - so it cannot deadlock on its own capacity.
//   A lease is finished on the thread that acquired it.
// - The pool starts no thread.
class ArchiveCursorPool
{
public:
    // A format's archive handle and decompressor state.
    class Cursor
    {
    public:
        virtual ~Cursor() = default;
    };

    // One extraction's ownership of a cursor slot. Move-only; ending it
    // (Finish or destruction) returns the capacity.
    class Lease
    {
    public:
        Lease() = default;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        // Closes the cursor (an exception or an uncertain state): Finish(false).
        ~Lease();

        // False when Acquire was stopped before capacity was granted.
        explicit operator bool() const { return m_pool != nullptr; }

        // The cursor: a retained one when Resumed(), otherwise null until the
        // caller opens one. The caller updates `position` as it reads.
        std::unique_ptr<Cursor> cursor {};
        size_t position { 0 };
        bool Resumed() const { return m_resumed; }

        // Closes a resumed cursor whose archive changed; the caller opens a new
        // one under the same lease (counted as an open).
        void Invalidate();
        // Refuses the archive (it changed since the reader scanned it).
        void CountInvalidated();
        void CountProgress(size_t headers, size_t entries);

        // Ends the lease. With `keep` the cursor becomes idle at `position`
        // (it must be at a valid boundary); otherwise it is closed.
        void Finish(bool keep);

    private:
        friend class ArchiveCursorPool;
        ArchiveCursorPool* m_pool { nullptr };
        std::uint64_t m_owner { 0 };
        std::uint64_t m_estimate { 0 };
        bool m_resumed { false };
    };

    explicit ArchiveCursorPool(ArchiveCursorLimits limits = {});
    ~ArchiveCursorPool();
    ArchiveCursorPool(const ArchiveCursorPool&) = delete;
    ArchiveCursorPool& operator=(const ArchiveCursorPool&) = delete;

    // The pool behind readers opened by ArchiveReaderFactory::Open (and so
    // VirtualFileSystem and ArchiveReaderCache::Shared()).
    static const std::shared_ptr<ArchiveCursorPool>& Shared();

    // A new reader identity for Acquire/ReleaseOwner.
    static std::uint64_t NewOwnerId();

    // A lease for reading `owner`'s archive from header `target` on. Waits
    // while the limits leave no room, until admitted or `stop` is requested
    // (then returns an empty lease). `memoryEstimate` is the reader's
    // per-cursor estimate.
    Lease Acquire(std::uint64_t owner, size_t target, std::uint64_t memoryEstimate, ExtractPriority priority, std::stop_token stop);

    // Closes every idle cursor of `owner` (its reader is gone or replaced).
    void ReleaseOwner(std::uint64_t owner);
    // Closes every idle cursor; returns how many.
    size_t TrimIdle();

    void SetLimits(const ArchiveCursorLimits& limits);
    ArchiveCursorLimits Limits() const;
    ArchiveCursorStats Stats() const;
    // Idle cursors of `owner` and their positions, most recently used first.
    std::vector<size_t> IdlePositions(std::uint64_t owner) const;

private:
    struct Idle
    {
        std::uint64_t owner { 0 };
        size_t position { 0 };
        std::uint64_t estimate { 0 };
        std::unique_ptr<Cursor> cursor {};
        std::chrono::steady_clock::time_point since {};
    };

    struct Waiter
    {
        std::uint64_t owner { 0 };
        size_t target { 0 };
        std::uint64_t estimate { 0 };
        ExtractPriority priority { ExtractPriority::Normal };
        bool granted { false };
        bool resumed { false };
        std::unique_ptr<Cursor> cursor {};   // adopted idle cursor
        size_t position { 0 };
    };

    using Doomed = std::vector<std::unique_ptr<Cursor>>;

    // Admits waiters in order while the limits allow it; closes idle cursors
    // (into `doomed`) to make memory room.
    void GrantLocked(Doomed& doomed);
    bool TryGrantLocked(Waiter& waiter, Doomed& doomed, bool& blocksOthers);
    void Admit(Waiter& waiter, std::list<Idle>::iterator idle);
    std::list<Idle>::iterator BestIdleLocked(const Waiter& waiter);
    void ExpireLocked(Doomed& doomed);
    void EnforceIdleLimitsLocked(Doomed& doomed);
    void EvictLocked(std::list<Idle>::iterator it, Doomed& doomed);
    void ReleaseLease(Lease& lease, bool keep);
    size_t ActiveOf(std::uint64_t owner) const;
    void NotePeaksLocked();

    mutable std::mutex m_mutex {};
    std::condition_variable_any m_changed {};
    ArchiveCursorLimits m_limits {};
    std::list<Idle> m_idle {};   // most recently used first
    std::list<Waiter*> m_highWaiters {};
    std::list<Waiter*> m_normalWaiters {};
    std::unordered_map<std::uint64_t, size_t> m_activePerOwner {};
    size_t m_active { 0 };
    std::uint64_t m_activeMemory { 0 };
    std::uint64_t m_idleMemory { 0 };
    ArchiveCursorStats m_stats {};
};
}
