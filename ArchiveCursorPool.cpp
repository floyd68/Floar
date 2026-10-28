#include "ArchiveCursorPool.h"

#include <algorithm>
#include <atomic>

namespace Floar
{
namespace
{
    // Leases held by this thread (any pool): a nested Acquire is admitted at
    // once so an entry callback cannot wait on its own capacity.
    thread_local size_t t_leasesHeld = 0;

    size_t NormalCap(size_t limit, size_t reservedForHigh)
    {
        return limit > reservedForHigh ? limit - reservedForHigh : 1;
    }

    ArchiveCursorLimits Sanitized(ArchiveCursorLimits limits)
    {
        limits.maxActive = (std::max)(limits.maxActive, size_t { 1 });
        limits.maxActivePerReader = (std::max)(limits.maxActivePerReader, size_t { 1 });
        return limits;
    }
}

// --- Lease ---

ArchiveCursorPool::Lease::Lease(Lease&& other) noexcept
    : cursor(std::move(other.cursor))
    , position(other.position)
    , m_pool(other.m_pool)
    , m_owner(other.m_owner)
    , m_estimate(other.m_estimate)
    , m_resumed(other.m_resumed)
{
    other.m_pool = nullptr;
}

ArchiveCursorPool::Lease& ArchiveCursorPool::Lease::operator=(Lease&& other) noexcept
{
    if (this != &other)
    {
        Finish(false);
        cursor = std::move(other.cursor);
        position = other.position;
        m_pool = other.m_pool;
        m_owner = other.m_owner;
        m_estimate = other.m_estimate;
        m_resumed = other.m_resumed;
        other.m_pool = nullptr;
    }
    return *this;
}

ArchiveCursorPool::Lease::~Lease()
{
    Finish(false);
}

void ArchiveCursorPool::Lease::Invalidate()
{
    cursor.reset();   // ours alone: closed outside the pool lock
    position = 0;
    m_resumed = false;
    if (m_pool)
    {
        std::lock_guard<std::mutex> lock(m_pool->m_mutex);
        ++m_pool->m_stats.invalidated;
        ++m_pool->m_stats.opens;
    }
}

void ArchiveCursorPool::Lease::CountInvalidated()
{
    if (m_pool)
    {
        std::lock_guard<std::mutex> lock(m_pool->m_mutex);
        ++m_pool->m_stats.invalidated;
    }
}

void ArchiveCursorPool::Lease::CountProgress(size_t headers, size_t entries)
{
    if (m_pool)
    {
        std::lock_guard<std::mutex> lock(m_pool->m_mutex);
        m_pool->m_stats.headersRead += headers;
        m_pool->m_stats.entriesRead += entries;
    }
}

void ArchiveCursorPool::Lease::Finish(bool keep)
{
    if (!m_pool)
    {
        cursor.reset();
        return;
    }
    m_pool->ReleaseLease(*this, keep);
}

// --- Pool ---

ArchiveCursorPool::ArchiveCursorPool(ArchiveCursorLimits limits)
    : m_limits(Sanitized(limits))
{
}

ArchiveCursorPool::~ArchiveCursorPool() = default;

const std::shared_ptr<ArchiveCursorPool>& ArchiveCursorPool::Shared()
{
    static const std::shared_ptr<ArchiveCursorPool> pool = std::make_shared<ArchiveCursorPool>();
    return pool;
}

std::uint64_t ArchiveCursorPool::NewOwnerId()
{
    static std::atomic<std::uint64_t> next { 0 };
    return ++next;
}

ArchiveCursorPool::Lease ArchiveCursorPool::Acquire(
    std::uint64_t owner, size_t target, std::uint64_t memoryEstimate, ExtractPriority priority, std::stop_token stop)
{
    Doomed doomed;
    Lease lease;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (stop.stop_requested())
        {
            return lease;
        }
        ExpireLocked(doomed);

        Waiter waiter;
        waiter.owner = owner;
        waiter.target = target;
        waiter.estimate = memoryEstimate;
        waiter.priority = priority;

        if (t_leasesHeld > 0)
        {
            // Nested in this thread's own extraction: admitted as usual when
            // the limits allow it, otherwise at once - never wait on ourselves.
            bool blocksOthers = false;
            if (!TryGrantLocked(waiter, doomed, blocksOthers))
            {
                Admit(waiter, BestIdleLocked(waiter));
            }
        }
        else
        {
            auto& queue = (priority == ExtractPriority::High) ? m_highWaiters : m_normalWaiters;
            queue.push_back(&waiter);
            GrantLocked(doomed);   // removes `waiter` from the queue once granted
            if (!waiter.granted)
            {
                ++m_stats.waits;
                if (!doomed.empty())
                {
                    lock.unlock();
                    doomed.clear();
                    lock.lock();
                }
                (void)m_changed.wait(lock, stop, [&] { return waiter.granted; });
            }
            if (!waiter.granted)
            {
                // Stopped while queued: nothing was granted, nothing to return.
                queue.remove(&waiter);
                ++m_stats.canceledWaits;
                GrantLocked(doomed);   // we may have been the head others waited behind
                m_changed.notify_all();
                lock.unlock();
                doomed.clear();
                return lease;
            }
        }

        ++t_leasesHeld;
        lease.m_pool = this;
        lease.m_owner = owner;
        lease.m_estimate = memoryEstimate;
        lease.m_resumed = waiter.resumed;
        lease.cursor = std::move(waiter.cursor);
        lease.position = waiter.position;
    }
    doomed.clear();
    return lease;
}

void ArchiveCursorPool::Admit(Waiter& waiter, std::list<Idle>::iterator idle)
{
    if (idle != m_idle.end())
    {
        waiter.cursor = std::move(idle->cursor);
        waiter.position = idle->position;
        waiter.resumed = true;
        m_idleMemory -= idle->estimate;
        m_idle.erase(idle);
        ++m_stats.resumes;
    }
    else
    {
        bool hasIdle = false;
        for (const Idle& entry : m_idle)
        {
            hasIdle = hasIdle || entry.owner == waiter.owner;
        }
        ++m_stats.opens;
        if (hasIdle)
        {
            ++m_stats.restarts;   // only cursors past the target remain
        }
    }
    m_activeMemory += waiter.estimate;
    ++m_active;
    ++m_activePerOwner[waiter.owner];
    waiter.granted = true;
    NotePeaksLocked();
}

bool ArchiveCursorPool::TryGrantLocked(Waiter& waiter, Doomed& doomed, bool& blocksOthers)
{
    const bool high = waiter.priority == ExtractPriority::High;
    const size_t capActive = high ? m_limits.maxActive : NormalCap(m_limits.maxActive, m_limits.reservedForHigh);
    const size_t capOwner = high ? m_limits.maxActivePerReader : NormalCap(m_limits.maxActivePerReader, m_limits.reservedForHigh);
    if (m_active >= capActive)
    {
        blocksOthers = true;   // FIFO: later waiters of this priority wait too
        return false;
    }
    if (ActiveOf(waiter.owner) >= capOwner)
    {
        blocksOthers = false;  // only this reader is full; others may proceed
        return false;
    }

    const auto best = BestIdleLocked(waiter);
    if (best != m_idle.end())
    {
        Admit(waiter, best);   // already counted in memory
        return true;
    }

    // Make memory room by closing least-recently-used idle cursors.
    while (m_activeMemory + m_idleMemory + waiter.estimate > m_limits.maxMemoryBytes && !m_idle.empty())
    {
        EvictLocked(std::prev(m_idle.end()), doomed);
    }
    if (!high && m_active > 0 && m_activeMemory + waiter.estimate > m_limits.maxMemoryBytes)
    {
        blocksOthers = true;
        return false;
    }
    Admit(waiter, m_idle.end());
    return true;
}

void ArchiveCursorPool::GrantLocked(Doomed& doomed)
{
    bool granted = false;
    for (auto* queue : { &m_highWaiters, &m_normalWaiters })
    {
        for (auto it = queue->begin(); it != queue->end();)
        {
            bool blocksOthers = false;
            if (TryGrantLocked(**it, doomed, blocksOthers))
            {
                it = queue->erase(it);
                granted = true;
                continue;
            }
            if (blocksOthers)
            {
                break;
            }
            ++it;
        }
    }
    if (granted)
    {
        m_changed.notify_all();
    }
}

void ArchiveCursorPool::ReleaseLease(Lease& lease, bool keep)
{
    Doomed doomed;
    std::unique_ptr<Cursor> cursor = std::move(lease.cursor);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        --m_active;
        if (auto it = m_activePerOwner.find(lease.m_owner); it != m_activePerOwner.end() && --it->second == 0)
        {
            m_activePerOwner.erase(it);
        }
        m_activeMemory -= lease.m_estimate;
        if (cursor)
        {
            if (keep)
            {
                Idle idle;
                idle.owner = lease.m_owner;
                idle.position = lease.position;
                idle.estimate = lease.m_estimate;
                idle.cursor = std::move(cursor);
                idle.since = std::chrono::steady_clock::now();
                m_idle.push_front(std::move(idle));
                m_idleMemory += lease.m_estimate;
                ++m_stats.retained;
            }
            else
            {
                ++m_stats.discarded;
                doomed.push_back(std::move(cursor));
            }
        }
        ExpireLocked(doomed);
        EnforceIdleLimitsLocked(doomed);
        GrantLocked(doomed);
        NotePeaksLocked();
        m_changed.notify_all();
    }
    --t_leasesHeld;
    lease.m_pool = nullptr;
    lease.position = 0;
    doomed.clear();   // closed outside the lock
}

void ArchiveCursorPool::EvictLocked(std::list<Idle>::iterator it, Doomed& doomed)
{
    m_idleMemory -= it->estimate;
    doomed.push_back(std::move(it->cursor));
    m_idle.erase(it);
    ++m_stats.evicted;
}

void ArchiveCursorPool::ExpireLocked(Doomed& doomed)
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_idle.begin(); it != m_idle.end();)
    {
        if (now - it->since >= m_limits.idleTimeout)
        {
            auto next = std::next(it);
            EvictLocked(it, doomed);
            it = next;
        }
        else
        {
            ++it;
        }
    }
}

void ArchiveCursorPool::EnforceIdleLimitsLocked(Doomed& doomed)
{
    // Per reader: keep each reader's most recently used ones.
    std::unordered_map<std::uint64_t, size_t> seen;
    for (auto it = m_idle.begin(); it != m_idle.end();)
    {
        if (++seen[it->owner] > m_limits.maxIdlePerReader)
        {
            auto next = std::next(it);
            EvictLocked(it, doomed);
            it = next;
        }
        else
        {
            ++it;
        }
    }
    while (m_idle.size() > m_limits.maxIdle)
    {
        EvictLocked(std::prev(m_idle.end()), doomed);
    }
    while (!m_idle.empty() && m_activeMemory + m_idleMemory > m_limits.maxMemoryBytes)
    {
        EvictLocked(std::prev(m_idle.end()), doomed);
    }
}

void ArchiveCursorPool::ReleaseOwner(std::uint64_t owner)
{
    Doomed doomed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_idle.begin(); it != m_idle.end();)
        {
            auto next = std::next(it);
            if (it->owner == owner)
            {
                EvictLocked(it, doomed);
            }
            it = next;
        }
        GrantLocked(doomed);
    }
    doomed.clear();
}

size_t ArchiveCursorPool::TrimIdle()
{
    Doomed doomed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_idle.empty())
        {
            EvictLocked(m_idle.begin(), doomed);
        }
        GrantLocked(doomed);
    }
    const size_t closed = doomed.size();
    doomed.clear();
    return closed;
}

void ArchiveCursorPool::SetLimits(const ArchiveCursorLimits& limits)
{
    Doomed doomed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_limits = Sanitized(limits);
        ExpireLocked(doomed);
        EnforceIdleLimitsLocked(doomed);
        GrantLocked(doomed);
    }
    doomed.clear();
}

ArchiveCursorLimits ArchiveCursorPool::Limits() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_limits;
}

ArchiveCursorStats ArchiveCursorPool::Stats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ArchiveCursorStats stats = m_stats;
    stats.active = m_active;
    stats.idle = m_idle.size();
    stats.waiting = m_highWaiters.size() + m_normalWaiters.size();
    stats.memoryBytes = m_activeMemory + m_idleMemory;
    return stats;
}

std::vector<size_t> ArchiveCursorPool::IdlePositions(std::uint64_t owner) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<size_t> positions;
    for (const Idle& idle : m_idle)
    {
        if (idle.owner == owner)
        {
            positions.push_back(idle.position);
        }
    }
    return positions;
}

std::list<ArchiveCursorPool::Idle>::iterator ArchiveCursorPool::BestIdleLocked(const Waiter& waiter)
{
    // The furthest retained cursor of this reader that is not past the target.
    auto best = m_idle.end();
    for (auto it = m_idle.begin(); it != m_idle.end(); ++it)
    {
        if (it->owner == waiter.owner && it->position <= waiter.target && (best == m_idle.end() || it->position > best->position))
        {
            best = it;
        }
    }
    return best;
}

size_t ArchiveCursorPool::ActiveOf(std::uint64_t owner) const
{
    const auto it = m_activePerOwner.find(owner);
    return it == m_activePerOwner.end() ? 0 : it->second;
}

void ArchiveCursorPool::NotePeaksLocked()
{
    m_stats.peakActive = (std::max)(m_stats.peakActive, m_active);
    m_stats.peakIdle = (std::max)(m_stats.peakIdle, m_idle.size());
    m_stats.peakMemoryBytes = (std::max)(m_stats.peakMemoryBytes, m_activeMemory + m_idleMemory);
}
}
