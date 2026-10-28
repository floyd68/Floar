#include "TestHarness.h"

#include "ArchiveCursorPool.h"

#include <atomic>
#include <cstdio>
#include <chrono>
#include <future>
#include <mutex>
#include <random>
#include <stop_token>
#include <thread>
#include <vector>

// ArchiveCursorPool with fake cursors: reuse of the furthest compatible
// cursor, restarts, FIFO and priority admission, cancelable waits that leak
// no capacity, active/idle/memory limits under concurrent load, idle expiry,
// and cursors never closed while in use or under the pool lock.

namespace
{
    using namespace Floar;
    using namespace std::chrono_literals;

    template <typename Predicate>
    bool WaitUntil(Predicate predicate, std::chrono::milliseconds bound = 10s)
    {
        const auto deadline = std::chrono::steady_clock::now() + bound;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    std::atomic<int> g_liveCursors { 0 };

    // Checks on destruction that it is not in use and that the pool lock is
    // not held (Stats takes it: a close under the lock would deadlock).
    class FakeCursor final : public ArchiveCursorPool::Cursor
    {
    public:
        explicit FakeCursor(ArchiveCursorPool* pool = nullptr)
            : m_pool(pool)
        {
            ++g_liveCursors;
        }

        ~FakeCursor() override
        {
            FLOAR_CHECK(!inUse.load());
            if (m_pool)
            {
                (void)m_pool->Stats();
            }
            --g_liveCursors;
        }

        std::atomic<bool> inUse { false };

    private:
        ArchiveCursorPool* m_pool;
    };

    ArchiveCursorLimits Limits(size_t maxActive, size_t perReader, size_t reserved)
    {
        ArchiveCursorLimits limits;
        limits.maxActive = maxActive;
        limits.maxActivePerReader = perReader;
        limits.reservedForHigh = reserved;
        return limits;
    }

    // Opens (or keeps) a cursor at `position` and returns it to the pool.
    void Retain(ArchiveCursorPool::Lease& lease, size_t position, ArchiveCursorPool* pool = nullptr)
    {
        if (!lease.cursor)
        {
            lease.cursor = std::make_unique<FakeCursor>(pool);
        }
        lease.position = position;
        lease.Finish(true);
    }
}

FLOAR_TEST(CursorPool_ResumesTheFurthestCompatibleCursor)
{
    ArchiveCursorPool pool;
    const auto owner = ArchiveCursorPool::NewOwnerId();
    const auto other = ArchiveCursorPool::NewOwnerId();

    auto lease = pool.Acquire(owner, 0, 10, ExtractPriority::Normal, {});
    FLOAR_REQUIRE(lease);
    FLOAR_CHECK(!lease.Resumed());
    FLOAR_CHECK(lease.cursor == nullptr);
    Retain(lease, 5);

    // Behind the cursor: a restart (the retained one stays).
    lease = pool.Acquire(owner, 3, 10, ExtractPriority::Normal, {});
    FLOAR_CHECK(!lease.Resumed());
    Retain(lease, 2);
    FLOAR_CHECK(pool.IdlePositions(owner) == (std::vector<size_t> { 2, 5 }));

    // Another reader never gets this reader's cursors.
    lease = pool.Acquire(other, 9, 10, ExtractPriority::Normal, {});
    FLOAR_CHECK(!lease.Resumed());
    lease.Finish(false);

    // At or after both: the furthest one, at its position.
    lease = pool.Acquire(owner, 7, 10, ExtractPriority::Normal, {});
    FLOAR_CHECK(lease.Resumed());
    FLOAR_CHECK(lease.position == 5);
    FLOAR_CHECK(lease.cursor != nullptr);
    Retain(lease, 8);
    lease = pool.Acquire(owner, 2, 10, ExtractPriority::Normal, {});
    FLOAR_CHECK(lease.Resumed());
    FLOAR_CHECK(lease.position == 2);
    lease.Finish(false);   // closed: an uncertain position

    const auto stats = pool.Stats();
    FLOAR_CHECK(stats.opens == 3);
    FLOAR_CHECK(stats.restarts == 1);
    FLOAR_CHECK(stats.resumes == 2);
    FLOAR_CHECK(stats.retained == 3);
    FLOAR_CHECK(stats.discarded == 1);
    FLOAR_CHECK(stats.active == 0);
    FLOAR_CHECK(stats.idle == 1);
    FLOAR_CHECK(stats.memoryBytes == 10);
    FLOAR_CHECK(pool.TrimIdle() == 1);
    FLOAR_CHECK(pool.Stats().memoryBytes == 0);
    FLOAR_CHECK(g_liveCursors == 0);
}

FLOAR_TEST(CursorPool_LeaseEndedWithoutFinishClosesItsCursor)
{
    ArchiveCursorPool pool;
    const auto owner = ArchiveCursorPool::NewOwnerId();
    {
        auto lease = pool.Acquire(owner, 0, 1, ExtractPriority::Normal, {});
        lease.cursor = std::make_unique<FakeCursor>(&pool);
        lease.position = 4;
        // An exception (or any early return) leaves the scope here.
    }
    FLOAR_CHECK(pool.Stats().active == 0);
    FLOAR_CHECK(pool.Stats().idle == 0);
    FLOAR_CHECK(pool.Stats().discarded == 1);

    // Moving a lease moves its ownership; only the target releases it.
    auto first = pool.Acquire(owner, 0, 1, ExtractPriority::Normal, {});
    ArchiveCursorPool::Lease moved = std::move(first);
    FLOAR_CHECK(!first);
    FLOAR_CHECK(moved);
    FLOAR_CHECK(pool.Stats().active == 1);
    moved.Finish(true);
    FLOAR_CHECK(pool.Stats().active == 0);
    FLOAR_CHECK(g_liveCursors == 0);
}

FLOAR_TEST(CursorPool_StoppedWaiterLeavesWithoutTakingCapacity)
{
    ArchiveCursorPool pool(Limits(1, 1, 0));
    const auto owner = ArchiveCursorPool::NewOwnerId();
    auto held = pool.Acquire(owner, 0, 1, ExtractPriority::Normal, {});
    FLOAR_REQUIRE(held);

    // Stopped before: returns at once, never queued.
    std::stop_source stopped;
    stopped.request_stop();
    FLOAR_CHECK(!pool.Acquire(owner, 0, 1, ExtractPriority::High, stopped.get_token()));
    FLOAR_CHECK(pool.Stats().waits == 0);

    std::stop_source stop;
    auto waiter = std::async(std::launch::async, [&] { return static_cast<bool>(pool.Acquire(owner, 0, 1, ExtractPriority::Normal, stop.get_token())); });
    FLOAR_REQUIRE(WaitUntil([&] { return pool.Stats().waiting == 1; }));
    stop.request_stop();
    FLOAR_REQUIRE(waiter.wait_for(10s) == std::future_status::ready);
    FLOAR_CHECK(!waiter.get());
    auto stats = pool.Stats();
    FLOAR_CHECK(stats.waiting == 0);
    FLOAR_CHECK(stats.canceledWaits == 1);
    FLOAR_CHECK(stats.active == 1);

    // The capacity is still the holder's alone, and free once it is done.
    held.Finish(true);
    FLOAR_CHECK(pool.Stats().active == 0);
    auto next = pool.Acquire(owner, 0, 1, ExtractPriority::Normal, {});
    FLOAR_CHECK(next);
    FLOAR_CHECK(pool.Stats().active == 1);
}

FLOAR_TEST(CursorPool_StoppedHeadWaiterDoesNotBlockTheQueue)
{
    ArchiveCursorPool pool(Limits(1, 1, 0));
    const auto a = ArchiveCursorPool::NewOwnerId();
    const auto b = ArchiveCursorPool::NewOwnerId();
    auto held = pool.Acquire(a, 0, 1, ExtractPriority::Normal, {});

    std::stop_source headStop;
    auto head = std::async(std::launch::async, [&] { return static_cast<bool>(pool.Acquire(a, 0, 1, ExtractPriority::Normal, headStop.get_token())); });
    FLOAR_REQUIRE(WaitUntil([&] { return pool.Stats().waiting == 1; }));
    std::promise<void> release;
    auto tail = std::async(std::launch::async, [&]
    {
        auto lease = pool.Acquire(b, 0, 1, ExtractPriority::Normal, {});
        release.get_future().wait();
        return static_cast<bool>(lease);
    });
    FLOAR_REQUIRE(WaitUntil([&] { return pool.Stats().waiting == 2; }));
    headStop.request_stop();
    FLOAR_REQUIRE(head.wait_for(10s) == std::future_status::ready);
    FLOAR_CHECK(!head.get());
    held.Finish(true);
    FLOAR_CHECK(WaitUntil([&] { return pool.Stats().active == 1 && pool.Stats().waiting == 0; }));
    release.set_value();
    FLOAR_REQUIRE(tail.wait_for(10s) == std::future_status::ready);
    FLOAR_CHECK(tail.get());
    FLOAR_CHECK(pool.Stats().active == 0);
}

FLOAR_TEST(CursorPool_HighPriorityUsesTheReservedCapacity)
{
    // Normal: 2 process-wide, 1 per reader; High: 3 and 2.
    ArchiveCursorPool pool(Limits(3, 2, 1));
    const auto a = ArchiveCursorPool::NewOwnerId();
    const auto b = ArchiveCursorPool::NewOwnerId();
    const auto c = ArchiveCursorPool::NewOwnerId();
    auto thumbA = pool.Acquire(a, 0, 1, ExtractPriority::Normal, {});
    auto thumbB = pool.Acquire(b, 0, 1, ExtractPriority::Normal, {});

    // A third Normal request waits (process-wide Normal limit) ...
    std::stop_source stop;
    auto normal = std::async(std::launch::async, [&] { return static_cast<bool>(pool.Acquire(c, 0, 1, ExtractPriority::Normal, stop.get_token())); });
    FLOAR_REQUIRE(WaitUntil([&] { return pool.Stats().waiting == 1; }));
    // ... while a main image of the same archive as a thumbnail gets in at once.
    auto main = pool.Acquire(a, 0, 1, ExtractPriority::High, {});
    FLOAR_CHECK(main);
    FLOAR_CHECK(pool.Stats().active == 3);
    FLOAR_CHECK(normal.wait_for(50ms) == std::future_status::timeout);

    main.Finish(true);
    // The freed slot is reserved for High: the Normal waiter still waits.
    FLOAR_CHECK(normal.wait_for(50ms) == std::future_status::timeout);
    thumbA.Finish(true);
    FLOAR_REQUIRE(normal.wait_for(10s) == std::future_status::ready);
    FLOAR_CHECK(normal.get());
    thumbB.Finish(true);
    FLOAR_CHECK(pool.Stats().active == 0);
}

FLOAR_TEST(CursorPool_AdmissionIsFifoWithHighFirst)
{
    ArchiveCursorPool pool(Limits(1, 1, 0));
    const auto owner = ArchiveCursorPool::NewOwnerId();
    auto held = pool.Acquire(owner, 0, 1, ExtractPriority::Normal, {});

    std::mutex mutex;
    std::vector<int> order;
    std::vector<std::thread> threads;
    const auto enqueue = [&](int id, ExtractPriority priority, size_t waitingAfter)
    {
        threads.emplace_back([&, id, priority]
        {
            auto lease = pool.Acquire(ArchiveCursorPool::NewOwnerId(), 0, 1, priority, {});
            {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(id);
            }
            lease.Finish(true);
        });
        FLOAR_CHECK(WaitUntil([&] { return pool.Stats().waiting == waitingAfter; }));
    };
    enqueue(1, ExtractPriority::Normal, 1);
    enqueue(2, ExtractPriority::Normal, 2);
    enqueue(3, ExtractPriority::High, 3);
    enqueue(4, ExtractPriority::Normal, 4);
    held.Finish(true);
    for (auto& thread : threads)
    {
        thread.join();
    }
    FLOAR_CHECK(order == (std::vector<int> { 3, 1, 2, 4 }));
}

FLOAR_TEST(CursorPool_ReaderLimitDoesNotBlockOtherReaders)
{
    ArchiveCursorPool pool(Limits(3, 2, 1));   // Normal: 1 per reader
    const auto a = ArchiveCursorPool::NewOwnerId();
    const auto b = ArchiveCursorPool::NewOwnerId();
    auto heldA = pool.Acquire(a, 0, 1, ExtractPriority::Normal, {});
    auto waitingA = std::async(std::launch::async, [&] { return static_cast<bool>(pool.Acquire(a, 0, 1, ExtractPriority::Normal, {})); });
    FLOAR_REQUIRE(WaitUntil([&] { return pool.Stats().waiting == 1; }));
    // Queued behind A's waiter, but only A is full.
    auto leaseB = pool.Acquire(b, 0, 1, ExtractPriority::Normal, {});
    FLOAR_CHECK(leaseB);
    leaseB.Finish(true);
    heldA.Finish(true);
    FLOAR_REQUIRE(waitingA.wait_for(10s) == std::future_status::ready);
    FLOAR_CHECK(waitingA.get());
}

FLOAR_TEST(CursorPool_MemoryBudgetClosesIdleCursorsAndBoundsNormalRequests)
{
    ArchiveCursorLimits limits = Limits(4, 2, 0);
    limits.maxMemoryBytes = 100;
    limits.maxIdle = 4;
    ArchiveCursorPool pool(limits);
    const auto a = ArchiveCursorPool::NewOwnerId();
    const auto b = ArchiveCursorPool::NewOwnerId();
    const auto c = ArchiveCursorPool::NewOwnerId();

    auto lease = pool.Acquire(a, 0, 40, ExtractPriority::Normal, {});
    Retain(lease, 1, &pool);
    lease = pool.Acquire(b, 0, 40, ExtractPriority::Normal, {});
    Retain(lease, 1, &pool);
    FLOAR_CHECK(pool.Stats().memoryBytes == 80);

    // 80 + 40 > 100: the least recently used idle cursor (A's) is closed.
    auto activeC = pool.Acquire(c, 0, 40, ExtractPriority::Normal, {});
    FLOAR_CHECK(pool.IdlePositions(a).empty());
    FLOAR_CHECK(pool.IdlePositions(b).size() == 1);
    FLOAR_CHECK(pool.Stats().memoryBytes == 80);
    auto activeA = pool.Acquire(a, 0, 40, ExtractPriority::Normal, {});
    FLOAR_CHECK(pool.Stats().idle == 0);   // B's closed too
    FLOAR_CHECK(pool.Stats().memoryBytes == 80);

    // Two active (80): a third Normal one (120) waits until one ends.
    auto waiter = std::async(std::launch::async, [&] { return static_cast<bool>(pool.Acquire(b, 0, 40, ExtractPriority::Normal, {})); });
    FLOAR_REQUIRE(WaitUntil([&] { return pool.Stats().waiting == 1; }));
    FLOAR_CHECK(waiter.wait_for(50ms) == std::future_status::timeout);
    activeA.Finish(false);
    FLOAR_REQUIRE(waiter.wait_for(10s) == std::future_status::ready);
    FLOAR_CHECK(waiter.get());
    activeC.Finish(false);
    FLOAR_CHECK(pool.Stats().peakMemoryBytes <= 100);

    // Alone, an estimate over the whole budget is admitted (one at a time).
    auto oversized = pool.Acquire(a, 0, 500, ExtractPriority::Normal, {});
    FLOAR_CHECK(oversized);
    oversized.Finish(false);
    FLOAR_CHECK(pool.Stats().memoryBytes == 0);
    FLOAR_CHECK(g_liveCursors == 0);
}

FLOAR_TEST(CursorPool_IdleLimitsAndExpiry)
{
    ArchiveCursorLimits limits = Limits(8, 8, 0);
    limits.maxIdle = 3;
    limits.maxIdlePerReader = 2;
    ArchiveCursorPool pool(limits);
    const auto a = ArchiveCursorPool::NewOwnerId();
    const auto b = ArchiveCursorPool::NewOwnerId();

    for (size_t position = 1; position <= 3; ++position)
    {
        auto lease = pool.Acquire(a, 0, 1, ExtractPriority::Normal, {});
        lease.cursor = std::make_unique<FakeCursor>(&pool);
        Retain(lease, position * 10);
    }
    // Per reader: its two most recent.
    FLOAR_CHECK(pool.IdlePositions(a) == (std::vector<size_t> { 30, 20 }));
    for (size_t position = 1; position <= 2; ++position)
    {
        auto lease = pool.Acquire(b, 0, 1, ExtractPriority::Normal, {});
        Retain(lease, position, &pool);
    }
    // Process-wide: three, least recently used closed first.
    FLOAR_CHECK(pool.Stats().idle == 3);
    FLOAR_CHECK(pool.IdlePositions(a) == (std::vector<size_t> { 30 }));
    FLOAR_CHECK(pool.Stats().peakIdle <= 3);

    // Expired cursors close at the next pool operation.
    std::this_thread::sleep_for(20ms);
    limits.idleTimeout = 10ms;
    pool.SetLimits(limits);
    FLOAR_CHECK(pool.Stats().idle == 0);
    FLOAR_CHECK(g_liveCursors == 0);

    // ReleaseOwner closes one reader's idle cursors only.
    limits.idleTimeout = 1h;
    pool.SetLimits(limits);
    auto lease = pool.Acquire(a, 0, 1, ExtractPriority::Normal, {});
    Retain(lease, 1, &pool);
    lease = pool.Acquire(b, 0, 1, ExtractPriority::Normal, {});
    Retain(lease, 1, &pool);
    pool.ReleaseOwner(a);
    FLOAR_CHECK(pool.IdlePositions(a).empty());
    FLOAR_CHECK(pool.IdlePositions(b).size() == 1);
    FLOAR_CHECK(pool.TrimIdle() == 1);
    FLOAR_CHECK(g_liveCursors == 0);
}

// A reader retired (ReleaseOwner: evicted or replaced) while an extraction
// still holds its cursor: the extraction decided to keep the cursor before
// the retirement and returns it after ReleaseOwner completed (the controlled
// interleaving below). The returned cursor is closed, never made idle, and
// so are the cursors of later leases of that reader, including a lease
// returned untouched right after it was acquired. ForgetOwner (the reader
// is destroyed) drops the record.
FLOAR_TEST(CursorPool_RetiredOwnerNeverRetainsAReturnedCursor)
{
    ArchiveCursorPool pool;
    const auto owner = ArchiveCursorPool::NewOwnerId();
    const auto other = ArchiveCursorPool::NewOwnerId();
    const int liveBefore = g_liveCursors.load();

    std::promise<void> leased;
    std::promise<void> retired;
    std::thread extraction([&]
    {
        auto lease = pool.Acquire(owner, 0, 1, ExtractPriority::Normal, {});
        lease.cursor = std::make_unique<FakeCursor>(&pool);
        lease.position = 3;
        const bool keep = true;   // sampled before the retirement
        leased.set_value();
        retired.get_future().wait();
        lease.Finish(keep);
    });
    leased.get_future().wait();
    FLOAR_CHECK(pool.Stats().active == 1);
    pool.ReleaseOwner(owner);   // completes while the cursor is leased
    retired.set_value();
    extraction.join();

    auto stats = pool.Stats();
    FLOAR_CHECK(pool.IdlePositions(owner).empty());
    FLOAR_CHECK(stats.idle == 0);
    FLOAR_CHECK(stats.active == 0);
    FLOAR_CHECK(stats.retained == 0);
    FLOAR_CHECK(stats.discarded == 1);
    FLOAR_CHECK(stats.memoryBytes == 0);
    FLOAR_CHECK(g_liveCursors == liveBefore);

    // Later leases of the retired reader retain nothing either.
    auto lease = pool.Acquire(owner, 0, 1, ExtractPriority::Normal, {});
    Retain(lease, 2, &pool);
    lease = pool.Acquire(owner, 0, 1, ExtractPriority::Normal, {});
    lease.Finish(true);   // canceled right after it was acquired
    FLOAR_CHECK(pool.IdlePositions(owner).empty());
    // Other readers are unaffected.
    lease = pool.Acquire(other, 0, 1, ExtractPriority::Normal, {});
    Retain(lease, 2, &pool);
    FLOAR_CHECK(pool.IdlePositions(other) == (std::vector<size_t> { 2 }));

    FLOAR_CHECK(pool.Stats().retiredOwners == 1);
    pool.ForgetOwner(owner);
    pool.ForgetOwner(other);
    stats = pool.Stats();
    FLOAR_CHECK(stats.retiredOwners == 0);
    FLOAR_CHECK(stats.idle == 0);
    FLOAR_CHECK(g_liveCursors == liveBefore);
}

FLOAR_TEST(CursorPool_NestedAcquireOnTheHoldingThreadDoesNotWait)
{
    ArchiveCursorPool pool(Limits(1, 1, 0));
    const auto owner = ArchiveCursorPool::NewOwnerId();
    auto outer = pool.Acquire(owner, 0, 1, ExtractPriority::Normal, {});
    // An entry callback extracting again on the same thread.
    auto inner = pool.Acquire(owner, 0, 1, ExtractPriority::Normal, {});
    FLOAR_CHECK(inner);
    FLOAR_CHECK(pool.Stats().active == 2);
    inner.Finish(true);
    outer.Finish(true);
    FLOAR_CHECK(pool.Stats().active == 0);
}

FLOAR_TEST(CursorPool_LimitsHoldUnderConcurrentLoad)
{
    ArchiveCursorLimits limits = Limits(3, 2, 1);
    limits.maxIdle = 2;
    limits.maxIdlePerReader = 1;
    ArchiveCursorPool pool(limits);
    const std::vector<std::uint64_t> owners = {
        ArchiveCursorPool::NewOwnerId(), ArchiveCursorPool::NewOwnerId(), ArchiveCursorPool::NewOwnerId() };

    std::atomic<int> normalActive { 0 };
    std::atomic<int> allActive { 0 };
    std::atomic<int> maxNormal { 0 };
    std::atomic<int> maxAll { 0 };
    std::vector<std::atomic<int>> perOwnerNormal(owners.size());
    std::atomic<int> perOwnerViolations { 0 };
    std::atomic<int> canceled { 0 };
    const auto noteMax = [](std::atomic<int>& peak, int value)
    {
        int seen = peak.load();
        while (value > seen && !peak.compare_exchange_weak(seen, value))
        {
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
    {
        threads.emplace_back([&, t]
        {
            std::mt19937 random(static_cast<unsigned>(t) + 7);
            for (int i = 0; i < 300; ++i)
            {
                const size_t which = random() % owners.size();
                const auto priority = (random() % 5 == 0) ? ExtractPriority::High : ExtractPriority::Normal;
                std::stop_source stop;
                if (random() % 10 == 0)
                {
                    stop.request_stop();   // some callers leave at once
                }
                auto lease = pool.Acquire(owners[which], random() % 50, 8, priority, stop.get_token());
                if (!lease)
                {
                    ++canceled;
                    continue;
                }
                if (!lease.cursor)
                {
                    lease.cursor = std::make_unique<FakeCursor>(&pool);
                }
                auto& cursor = static_cast<FakeCursor&>(*lease.cursor);
                // Exclusive: nobody else holds this cursor now.
                FLOAR_CHECK(!cursor.inUse.exchange(true));
                noteMax(maxAll, ++allActive);
                if (priority == ExtractPriority::Normal)
                {
                    noteMax(maxNormal, ++normalActive);
                    if (++perOwnerNormal[which] > 1)
                    {
                        ++perOwnerViolations;
                    }
                }
                std::this_thread::yield();
                if (priority == ExtractPriority::Normal)
                {
                    --perOwnerNormal[which];
                    --normalActive;
                }
                --allActive;
                cursor.inUse = false;
                lease.position = random() % 60;
                lease.Finish(random() % 4 != 0);
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    const auto stats = pool.Stats();
    std::printf("  peak active %zu (normal %d), peak idle %zu, opens %llu, resumes %llu, waits %llu\n",
        stats.peakActive, maxNormal.load(), stats.peakIdle,
        static_cast<unsigned long long>(stats.opens), static_cast<unsigned long long>(stats.resumes),
        static_cast<unsigned long long>(stats.waits));
    FLOAR_CHECK(maxAll <= 3);
    FLOAR_CHECK(maxNormal <= 2);
    FLOAR_CHECK(perOwnerViolations == 0);
    FLOAR_CHECK(stats.peakActive <= 3);
    FLOAR_CHECK(stats.peakIdle <= 2);
    FLOAR_CHECK(stats.peakMemoryBytes <= (3 + 2) * 8);
    FLOAR_CHECK(stats.active == 0);
    FLOAR_CHECK(stats.waiting == 0);
    FLOAR_CHECK(stats.idle <= 2);
    for (const auto owner : owners)
    {
        FLOAR_CHECK(pool.IdlePositions(owner).size() <= 1);
    }
    FLOAR_CHECK(stats.resumes > 0);
    FLOAR_CHECK(stats.waits > 0);
    FLOAR_CHECK(canceled > 0);
    (void)pool.TrimIdle();
    FLOAR_CHECK(g_liveCursors == 0);
}
