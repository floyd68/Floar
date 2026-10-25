#include "TestHarness.h"

#include "ArchiveReader.h"
#include "ArchiveReaderCache.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// The archive-reader cache's open-worker pool, driven by injected openers:
// lazy worker start, shared opens with interest tracking (a stopped caller
// never stops an open others wait for), retirement when the last interest
// leaves, FIFO admission bounded by the worker count, bounded Shutdown, and
// best-effort OS cancellation of an open blocked in a synchronous pipe read.
// Every wait is bounded; gates and counters replace sleeps.

namespace
{
    using namespace Floar;
    using namespace std::chrono_literals;

    std::filesystem::path TempDir(const std::wstring& tag)
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        const std::filesystem::path dir = std::filesystem::path(tempPath) /
            (L"FloarPool_" + std::to_wstring(GetCurrentProcessId()) + L"_" + tag + L"_" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(dir);
        return dir;
    }

    struct ScopedDir
    {
        std::filesystem::path path;

        std::filesystem::path File(const std::wstring& name) const
        {
            const std::filesystem::path file = path / name;
            std::ofstream(file, std::ios::binary | std::ios::trunc) << "x";
            return file;
        }

        ~ScopedDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    // Polls `condition` until it holds or `timeout` passes.
    bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 5s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    template <typename T>
    bool Ready(std::future<T>& future, std::chrono::milliseconds timeout = 5s)
    {
        return future.wait_for(timeout) == std::future_status::ready;
    }

    class GenReader final : public IArchiveReader
    {
    public:
        explicit GenReader(int generation)
            : generation(generation)
        {
        }

        const std::vector<ArchiveEntry>& Entries() const override { return m_entries; }
        std::vector<uint8_t> ExtractToMemory(const std::wstring&) override { return {}; }
        bool HasEntry(const std::wstring&) override { return false; }
        bool HasDirectory(const std::wstring&) const override { return false; }
        std::wstring GetFormatName() const override { return L"gen"; }

        const int generation;

    private:
        std::vector<ArchiveEntry> m_entries {};
    };

    // Opener whose calls block until released per archive (file name).
    // `honorStop` false: the open ignores its stop token (a stuck backend).
    class GatedOpener
    {
    public:
        struct Call
        {
            std::wstring name {};
            bool stopSeen { false };
            bool returned { false };
        };

        bool honorStop { true };
        // A stopped open still returns a reader, to show it is not published.
        bool readerWhenStopped { false };

        ArchiveReaderCache::Opener Make()
        {
            return [this](const std::filesystem::path& path, std::stop_token stop) -> std::shared_ptr<IArchiveReader>
            {
                const std::wstring name = path.filename().wstring();
                std::unique_lock<std::mutex> lock(m_mutex);
                const size_t index = m_calls.size();
                m_calls.push_back({ name });
                const int generation = static_cast<int>(index) + 1;
                ++m_concurrent;
                m_maxConcurrent = (std::max)(m_maxConcurrent, m_concurrent);
                m_changed.notify_all();
                const auto released = [&] { return m_released[name] > 0 || m_releaseAll || m_releasedCalls.count(index) != 0; };
                if (honorStop)
                {
                    (void)m_changed.wait(lock, stop, released);
                }
                else
                {
                    m_changed.wait(lock, released);
                }
                const bool stopped = stop.stop_requested();
                if (!stopped && m_releasedCalls.count(index) == 0 && m_released[name] > 0)
                {
                    --m_released[name];
                }
                m_calls[index].stopSeen = stopped;
                m_calls[index].returned = true;
                --m_concurrent;
                m_changed.notify_all();
                if (stopped && !readerWhenStopped)
                {
                    return nullptr;
                }
                return std::make_shared<GenReader>(generation);
            };
        }

        void Release(const std::wstring& name)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_released[name];
            m_changed.notify_all();
        }

        // Releases one call (by its index in Calls()).
        void ReleaseCall(size_t index)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_releasedCalls.insert(index);
            m_changed.notify_all();
        }

        void ReleaseAll()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_releaseAll = true;
            m_changed.notify_all();
        }

        bool WaitCalls(size_t count, std::chrono::milliseconds timeout = 5s)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_changed.wait_for(lock, timeout, [&] { return m_calls.size() >= count; });
        }

        bool WaitReturned(size_t index, std::chrono::milliseconds timeout = 5s)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_changed.wait_for(lock, timeout, [&] { return m_calls.size() > index && m_calls[index].returned; });
        }

        std::vector<Call> Calls()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_calls;
        }

        size_t MaxConcurrent()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_maxConcurrent;
        }

    private:
        std::mutex m_mutex {};
        std::condition_variable_any m_changed {};
        std::vector<Call> m_calls {};
        std::map<std::wstring, int> m_released {};
        std::set<size_t> m_releasedCalls {};
        bool m_releaseAll { false };
        size_t m_concurrent { 0 };
        size_t m_maxConcurrent { 0 };
    };

    std::future<std::shared_ptr<IArchiveReader>> GetAsync(ArchiveReaderCache& cache, const std::filesystem::path& path, std::stop_token stop = {})
    {
        return std::async(std::launch::async, [&cache, path, stop] { return cache.Get(path, stop); });
    }

    int Generation(const std::shared_ptr<IArchiveReader>& reader)
    {
        const auto* gen = dynamic_cast<const GenReader*>(reader.get());
        return gen ? gen->generation : 0;
    }
}

FLOAR_TEST(Pool_NoWorkerStartsBeforeFirstOpenAndShutdownIsRepeatable)
{
    ScopedDir dir { TempDir(L"lazy") };
    const auto a = dir.File(L"a.zip");
    GatedOpener opener;
    ArchiveReaderCache cache(4, opener.Make());
    FLOAR_CHECK(cache.WorkerCount() == 0);
    FLOAR_CHECK(cache.Shutdown(0ms));
    FLOAR_CHECK(cache.WorkerCount() == 0);
    FLOAR_CHECK(cache.Shutdown(0ms));
    // After shutdown nothing is admitted and nothing starts.
    FLOAR_CHECK(cache.Get(a) == nullptr);
    FLOAR_CHECK(cache.WorkerCount() == 0);
    FLOAR_CHECK(cache.OpenCount() == 0);
    FLOAR_CHECK(opener.Calls().empty());
}

FLOAR_TEST(Pool_PreCanceledGetStartsNoOpen)
{
    ScopedDir dir { TempDir(L"precancel") };
    const auto a = dir.File(L"a.zip");
    GatedOpener opener;
    ArchiveReaderCache cache(4, opener.Make());
    std::stop_source stop;
    stop.request_stop();
    FLOAR_CHECK(cache.Get(a, stop.get_token()) == nullptr);
    FLOAR_CHECK(cache.WorkerCount() == 0);
    FLOAR_CHECK(cache.OpenCount() == 0);
    FLOAR_CHECK(cache.Size() == 0);
    FLOAR_CHECK(cache.Shutdown(5s));
}

FLOAR_TEST(Pool_InitiatorCancelsWhileTheSharedOpenContinuesForOthers)
{
    ScopedDir dir { TempDir(L"share") };
    const auto a = dir.File(L"a.zip");
    GatedOpener opener;
    ArchiveReaderCache cache(4, opener.Make());

    std::stop_source stopA;
    auto callerA = GetAsync(cache, a, stopA.get_token());
    FLOAR_REQUIRE(opener.WaitCalls(1));
    std::stop_source stopB;
    auto callerB = GetAsync(cache, a, stopB.get_token());
    auto callerC = GetAsync(cache, a);   // untokened, e.g. a main-image read
    FLOAR_REQUIRE(WaitUntil([&] { return cache.WaitingCount() == 3; }));
    FLOAR_CHECK(cache.RunningOpenCount() == 1);
    FLOAR_CHECK(cache.QueuedOpenCount() == 0);

    // A initiated the open; it leaves without waiting for the scan.
    stopA.request_stop();
    FLOAR_REQUIRE(Ready(callerA));
    FLOAR_CHECK(callerA.get() == nullptr);
    FLOAR_CHECK(cache.WaitingCount() == 2);
    // The open was not stopped on B's and C's behalf.
    FLOAR_CHECK(!opener.Calls()[0].stopSeen);
    FLOAR_CHECK(!opener.Calls()[0].returned);

    opener.Release(L"a.zip");
    FLOAR_REQUIRE(Ready(callerB));
    FLOAR_REQUIRE(Ready(callerC));
    const auto reader = callerB.get();
    FLOAR_CHECK(reader != nullptr);
    FLOAR_CHECK(callerC.get() == reader);
    FLOAR_CHECK(cache.OpenCount() == 1);
    // Later callers (also stopped ones) reuse the reader without a rescan.
    FLOAR_CHECK(cache.Get(a, stopA.get_token()) == reader);
    FLOAR_CHECK(cache.OpenCount() == 1);
    FLOAR_CHECK(cache.Shutdown(5s));
}

FLOAR_TEST(Pool_LastInterestLeavingStopsTheOpenAndPublishesNothing)
{
    ScopedDir dir { TempDir(L"retire") };
    const auto a = dir.File(L"a.zip");
    GatedOpener opener;
    opener.readerWhenStopped = true;
    ArchiveReaderCache cache(4, opener.Make());

    std::stop_source stopA;
    std::stop_source stopB;
    auto callerA = GetAsync(cache, a, stopA.get_token());
    FLOAR_REQUIRE(opener.WaitCalls(1));
    auto callerB = GetAsync(cache, a, stopB.get_token());
    FLOAR_REQUIRE(WaitUntil([&] { return cache.WaitingCount() == 2; }));

    stopA.request_stop();
    FLOAR_REQUIRE(Ready(callerA));
    FLOAR_CHECK(!opener.Calls()[0].stopSeen);
    stopB.request_stop();
    FLOAR_REQUIRE(Ready(callerB));
    FLOAR_CHECK(callerA.get() == nullptr);
    FLOAR_CHECK(callerB.get() == nullptr);

    // The last interest left: the open is stopped, and the reader it still
    // returned is discarded.
    FLOAR_REQUIRE(opener.WaitReturned(0));
    FLOAR_CHECK(opener.Calls()[0].stopSeen);
    FLOAR_REQUIRE(WaitUntil([&] { return cache.RunningOpenCount() == 0; }));
    FLOAR_CHECK(cache.Size() == 0);

    // A new caller starts a replacement open instead of inheriting the stopped one.
    opener.Release(L"a.zip");
    const auto reader = cache.Get(a);
    FLOAR_CHECK(Generation(reader) == 2);
    FLOAR_CHECK(cache.OpenCount() == 2);
    FLOAR_CHECK(cache.Shutdown(5s));
}

FLOAR_TEST(Pool_CallerRacingTheLastInterestGetsAValidGeneration)
{
    ScopedDir dir { TempDir(L"race") };
    const auto a = dir.File(L"a.zip");
    for (int round = 0; round < 100; ++round)
    {
        GatedOpener opener;
        ArchiveReaderCache cache(4, opener.Make());
        std::stop_source stopA;
        auto callerA = GetAsync(cache, a, stopA.get_token());
        FLOAR_REQUIRE(opener.WaitCalls(1));

        // B arrives while A's interest leaves: it either joins the first open
        // (which then is not stopped) or starts a replacement.
        std::promise<void> go;
        std::shared_future<void> start = go.get_future().share();
        auto stopper = std::async(std::launch::async, [&] { start.wait(); stopA.request_stop(); });
        auto callerB = std::async(std::launch::async, [&] { start.wait(); return cache.Get(a); });
        go.set_value();
        FLOAR_REQUIRE(Ready(stopper));
        FLOAR_REQUIRE(Ready(callerA));
        opener.ReleaseAll();
        FLOAR_REQUIRE(Ready(callerB));

        const auto reader = callerB.get();
        FLOAR_REQUIRE(reader != nullptr);
        const auto calls = opener.Calls();
        const int generation = Generation(reader);
        FLOAR_REQUIRE(generation >= 1 && generation <= static_cast<int>(calls.size()));
        FLOAR_CHECK(!calls[generation - 1].stopSeen);
        FLOAR_CHECK(cache.Get(a) == reader);   // the published reader
        FLOAR_CHECK(cache.Shutdown(5s));
    }
}

FLOAR_TEST(Pool_QueuedOpensWaitInFifoOrderWithoutTakingWorkers)
{
    ScopedDir dir { TempDir(L"fifo") };
    const auto x = dir.File(L"x.zip");
    const auto y = dir.File(L"y.zip");
    const auto z1 = dir.File(L"z1.zip");
    const auto z2 = dir.File(L"z2.zip");
    const auto z3 = dir.File(L"z3.zip");
    GatedOpener opener;
    ArchiveReaderCache cache(8, opener.Make(), 2);

    auto callerX = GetAsync(cache, x);
    auto callerY = GetAsync(cache, y);
    FLOAR_REQUIRE(opener.WaitCalls(2));
    FLOAR_CHECK(cache.WorkerCount() == 2);
    FLOAR_CHECK(cache.RunningOpenCount() == 2);

    std::stop_source stopZ1;
    auto callerZ1 = GetAsync(cache, z1, stopZ1.get_token());
    FLOAR_REQUIRE(WaitUntil([&] { return cache.QueuedOpenCount() == 1; }));
    auto callerZ2 = GetAsync(cache, z2);   // untokened
    FLOAR_REQUIRE(WaitUntil([&] { return cache.QueuedOpenCount() == 2; }));
    std::stop_source stopZ3;
    auto callerZ3 = GetAsync(cache, z3, stopZ3.get_token());
    FLOAR_REQUIRE(WaitUntil([&] { return cache.QueuedOpenCount() == 3; }));
    // A second caller of a queued archive shares its queued operation.
    auto callerZ1b = GetAsync(cache, z1);
    FLOAR_REQUIRE(WaitUntil([&] { return cache.WaitingCount() == 6; }));
    FLOAR_CHECK(cache.QueuedOpenCount() == 3);
    FLOAR_CHECK(cache.WorkerCount() == 2);
    FLOAR_CHECK(cache.RunningOpenCount() == 2);

    // A stopped tokened waiter leaves the queue without taking a slot.
    stopZ3.request_stop();
    FLOAR_REQUIRE(Ready(callerZ3));
    FLOAR_CHECK(callerZ3.get() == nullptr);
    FLOAR_CHECK(cache.QueuedOpenCount() == 2);
    FLOAR_CHECK(cache.OpenCount() == 2);

    // Capacity released by X admits Z1, then Y's admits Z2.
    opener.Release(L"x.zip");
    FLOAR_REQUIRE(opener.WaitCalls(3));
    FLOAR_CHECK(opener.Calls()[2].name == L"z1.zip");
    FLOAR_REQUIRE(WaitUntil([&] { return cache.QueuedOpenCount() == 1; }));
    FLOAR_CHECK(cache.RunningOpenCount() == 2);
    opener.Release(L"y.zip");
    FLOAR_REQUIRE(opener.WaitCalls(4));
    FLOAR_CHECK(opener.Calls()[3].name == L"z2.zip");
    FLOAR_CHECK(cache.QueuedOpenCount() == 0);
    FLOAR_CHECK(cache.WorkerCount() == 2);

    opener.Release(L"z1.zip");
    opener.Release(L"z2.zip");
    for (auto* caller : { &callerX, &callerY, &callerZ1, &callerZ1b, &callerZ2 })
    {
        FLOAR_REQUIRE(Ready(*caller));
    }
    FLOAR_CHECK(callerX.get() != nullptr);
    FLOAR_CHECK(callerY.get() != nullptr);
    const auto readerZ1 = callerZ1.get();
    FLOAR_CHECK(readerZ1 != nullptr);
    FLOAR_CHECK(callerZ1b.get() == readerZ1);
    FLOAR_CHECK(callerZ2.get() != nullptr);
    FLOAR_CHECK(cache.OpenCount() == 4);   // z3 never opened, z1 opened once
    FLOAR_CHECK(opener.MaxConcurrent() <= 2);
    FLOAR_CHECK(cache.Shutdown(5s));
}

FLOAR_TEST(Pool_ShutdownStopsOpensAndReleasesEveryWaiter)
{
    ScopedDir dir { TempDir(L"shutdown") };
    const auto x = dir.File(L"x.zip");
    const auto y = dir.File(L"y.zip");
    const auto z1 = dir.File(L"z1.zip");
    const auto z2 = dir.File(L"z2.zip");
    GatedOpener opener;
    opener.readerWhenStopped = true;
    ArchiveReaderCache cache(8, opener.Make(), 2);

    auto callerX = GetAsync(cache, x);
    auto callerY = GetAsync(cache, y);
    FLOAR_REQUIRE(opener.WaitCalls(2));
    std::stop_source stopZ1;
    auto callerZ1 = GetAsync(cache, z1, stopZ1.get_token());
    auto callerZ2 = GetAsync(cache, z2);
    FLOAR_REQUIRE(WaitUntil([&] { return cache.QueuedOpenCount() == 2 && cache.WaitingCount() == 4; }));

    FLOAR_CHECK(cache.Shutdown(5s));
    for (auto* caller : { &callerX, &callerY, &callerZ1, &callerZ2 })
    {
        FLOAR_REQUIRE(Ready(*caller));
        FLOAR_CHECK(caller->get() == nullptr);
    }
    const auto calls = opener.Calls();
    FLOAR_CHECK(calls.size() == 2);   // the queued opens never started
    FLOAR_CHECK(calls[0].stopSeen && calls[1].stopSeen);
    FLOAR_CHECK(cache.WorkerCount() == 0);
    FLOAR_CHECK(cache.Size() == 0);
    // Get after Shutdown starts nothing.
    FLOAR_CHECK(cache.Get(x) == nullptr);
    FLOAR_CHECK(cache.OpenCount() == 2);
    FLOAR_CHECK(cache.WorkerCount() == 0);
    FLOAR_CHECK(cache.Shutdown(0ms));
}

FLOAR_TEST(Pool_ShutdownTimesOutOnAStuckOpenAndARetryCompletes)
{
    ScopedDir dir { TempDir(L"stuck") };
    const auto a = dir.File(L"a.zip");
    const auto b = dir.File(L"b.zip");
    GatedOpener opener;
    opener.honorStop = false;
    ArchiveReaderCache cache(4, opener.Make());

    auto caller = GetAsync(cache, a);
    FLOAR_REQUIRE(opener.WaitCalls(1));
    FLOAR_CHECK(!cache.Shutdown(50ms));
    // The waiter is released even though the worker is stuck; the worker is kept.
    FLOAR_REQUIRE(Ready(caller));
    FLOAR_CHECK(caller.get() == nullptr);
    FLOAR_CHECK(cache.WorkerCount() == 1);
    FLOAR_CHECK(cache.Get(b) == nullptr);
    FLOAR_CHECK(cache.OpenCount() == 1);
    FLOAR_CHECK(!cache.Shutdown(10ms));

    opener.ReleaseAll();
    FLOAR_CHECK(cache.Shutdown(5s));
    FLOAR_CHECK(cache.WorkerCount() == 0);
    FLOAR_CHECK(cache.Size() == 0);   // the late reader was not published
    FLOAR_CHECK(opener.Calls().size() == 1);
}

FLOAR_TEST(Pool_ShutdownRacingTheLastInterestLeavesNothingBehind)
{
    ScopedDir dir { TempDir(L"shutdown-race") };
    const auto a = dir.File(L"a.zip");
    for (int round = 0; round < 100; ++round)
    {
        GatedOpener opener;
        opener.readerWhenStopped = true;
        ArchiveReaderCache cache(4, opener.Make());
        std::stop_source stopA;
        auto callerA = GetAsync(cache, a, stopA.get_token());
        FLOAR_REQUIRE(opener.WaitCalls(1));

        std::promise<void> go;
        std::shared_future<void> start = go.get_future().share();
        auto stopper = std::async(std::launch::async, [&] { start.wait(); stopA.request_stop(); });
        auto shutdown = std::async(std::launch::async, [&] { start.wait(); return cache.Shutdown(5s); });
        go.set_value();
        FLOAR_REQUIRE(Ready(stopper));
        FLOAR_REQUIRE(Ready(shutdown, 10s));
        FLOAR_CHECK(shutdown.get());
        FLOAR_REQUIRE(Ready(callerA));
        FLOAR_CHECK(callerA.get() == nullptr);
        FLOAR_CHECK(opener.Calls()[0].stopSeen);
        FLOAR_CHECK(cache.WorkerCount() == 0);
        FLOAR_CHECK(cache.Size() == 0);
        FLOAR_CHECK(cache.Get(a) == nullptr);
    }
}

FLOAR_TEST(Pool_ChurnWithCancellationStaysWithinTheWorkerBound)
{
    ScopedDir dir { TempDir(L"churn") };
    std::vector<std::filesystem::path> archives;
    for (int i = 0; i < 6; ++i)
    {
        archives.push_back(dir.File(L"c" + std::to_wstring(i) + L".zip"));
    }

    std::atomic<int> concurrent { 0 };
    std::atomic<int> maxConcurrent { 0 };
    std::atomic<int> generation { 0 };
    // A short cooperative open.
    ArchiveReaderCache cache(2, [&](const std::filesystem::path&, std::stop_token stop) -> std::shared_ptr<IArchiveReader>
    {
        const int now = ++concurrent;
        int seen = maxConcurrent.load();
        while (now > seen && !maxConcurrent.compare_exchange_weak(seen, now))
        {
        }
        std::mutex mutex;
        std::condition_variable_any never;
        std::unique_lock<std::mutex> lock(mutex);
        (void)never.wait_for(lock, stop, 2ms, [] { return false; });
        --concurrent;
        return stop.stop_requested() ? nullptr : std::make_shared<GenReader>(++generation);
    }, 2);

    std::mutex sourcesMutex;
    std::vector<std::stop_source> current(6);
    std::atomic<bool> done { false };
    std::atomic<size_t> maxWorkers { 0 };
    std::thread chaos([&]
    {
        std::mt19937 random(7);
        while (!done)
        {
            {
                std::lock_guard<std::mutex> lock(sourcesMutex);
                current[random() % current.size()].request_stop();
            }
            const size_t workers = cache.WorkerCount();
            if (workers > maxWorkers)
            {
                maxWorkers = workers;
            }
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> callers;
    for (int t = 0; t < 6; ++t)
    {
        callers.emplace_back([&, t]
        {
            std::mt19937 random(static_cast<unsigned>(t));
            for (int i = 0; i < 150; ++i)
            {
                std::stop_source stop;
                {
                    std::lock_guard<std::mutex> lock(sourcesMutex);
                    current[t] = stop;
                }
                const auto& path = archives[random() % archives.size()];
                (void)cache.Get(path, (i % 4 == 0) ? std::stop_token {} : stop.get_token());
            }
        });
    }
    for (auto& caller : callers)
    {
        caller.join();
    }
    done = true;
    chaos.join();

    FLOAR_CHECK(maxConcurrent <= 2);
    FLOAR_CHECK(maxWorkers <= 2);
    FLOAR_CHECK(cache.WorkerCount() <= 2);
    FLOAR_CHECK(WaitUntil([&] { return cache.RunningOpenCount() == 0; }));
    FLOAR_CHECK(cache.QueuedOpenCount() == 0);
    FLOAR_CHECK(cache.WaitingCount() == 0);
    FLOAR_CHECK(cache.Size() <= 2);
    FLOAR_CHECK(cache.Shutdown(5s));
}

namespace
{
    // Opener whose open of an archive blocks in a synchronous ReadFile on that
    // archive's anonymous pipe and ignores its stop token: only
    // CancelSynchronousIo (or data on the pipe) releases it.
    class PipeOpener
    {
    public:
        struct Pipe
        {
            HANDLE read { nullptr };
            HANDLE write { nullptr };
            std::atomic<bool> entered { false };
            std::atomic<bool> aborted { false };
            std::atomic<bool> returned { false };
            std::atomic<DWORD> threadId { 0 };
            HANDLE thread { nullptr };
        };

        explicit PipeOpener(const std::vector<std::wstring>& names)
        {
            for (const auto& name : names)
            {
                auto pipe = std::make_unique<Pipe>();
                (void)CreatePipe(&pipe->read, &pipe->write, nullptr, 0);
                m_pipes.emplace(name, std::move(pipe));
            }
        }

        ~PipeOpener()
        {
            for (auto& [name, pipe] : m_pipes)
            {
                for (HANDLE handle : { pipe->read, pipe->write, pipe->thread })
                {
                    if (handle != nullptr)
                    {
                        CloseHandle(handle);
                    }
                }
            }
        }

        Pipe& operator[](const std::wstring& name) { return *m_pipes.at(name); }

        ArchiveReaderCache::Opener Make()
        {
            return [this](const std::filesystem::path& path, std::stop_token) -> std::shared_ptr<IArchiveReader>
            {
                Pipe& pipe = (*this)[path.filename().wstring()];
                pipe.thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
                pipe.threadId = GetCurrentThreadId();
                pipe.entered = true;
                char byte = 0;
                DWORD read = 0;
                const BOOL ok = ::ReadFile(pipe.read, &byte, 1, &read, nullptr);
                pipe.aborted = !ok && GetLastError() == ERROR_OPERATION_ABORTED;
                pipe.returned = true;
                return ok ? std::make_shared<GenReader>(1) : nullptr;
            };
        }

        // Waits until the opener of `name` is blocked in its read: its thread
        // has entered and stopped consuming CPU cycles.
        bool WaitBlocked(const std::wstring& name)
        {
            Pipe& pipe = (*this)[name];
            if (!WaitUntil([&] { return pipe.entered.load(); }))
            {
                return false;
            }
            return WaitUntil([&]
            {
                ULONG64 before = 0;
                ULONG64 after = 0;
                if (!QueryThreadCycleTime(pipe.thread, &before))
                {
                    return false;
                }
                std::this_thread::sleep_for(20ms);
                return QueryThreadCycleTime(pipe.thread, &after) && before == after && !pipe.returned;
            });
        }

        // Unblocks a read the cancellation did not reach (so a failing test cannot hang).
        void Unblock(const std::wstring& name)
        {
            DWORD written = 0;
            (void)WriteFile((*this)[name].write, "x", 1, &written, nullptr);
        }

    private:
        std::map<std::wstring, std::unique_ptr<Pipe>> m_pipes {};
    };
}

FLOAR_TEST(Pool_BlockedOsReadIsCancelledWhenTheLastInterestLeaves)
{
    ScopedDir dir { TempDir(L"pipe") };
    const auto x = dir.File(L"x.zip");
    const auto next = dir.File(L"next.zip");
    PipeOpener opener({ L"x.zip", L"next.zip" });
    ArchiveReaderCache cache(4, opener.Make(), 1);

    std::stop_source stopX;
    auto callerX = GetAsync(cache, x, stopX.get_token());
    FLOAR_REQUIRE(opener.WaitBlocked(L"x.zip"));
    auto callerNext = GetAsync(cache, next);
    FLOAR_REQUIRE(WaitUntil([&] { return cache.QueuedOpenCount() == 1; }));
    FLOAR_CHECK(cache.WorkerCount() == 1);

    stopX.request_stop();
    FLOAR_REQUIRE(Ready(callerX));
    FLOAR_CHECK(callerX.get() == nullptr);
    const bool released = WaitUntil([&] { return opener[L"x.zip"].returned.load(); });
    if (!released)
    {
        opener.Unblock(L"x.zip");
    }
    FLOAR_CHECK(released);
    FLOAR_CHECK(opener[L"x.zip"].aborted);

    // The freed worker admits the queued open; the cancel does not reach it.
    FLOAR_REQUIRE(opener.WaitBlocked(L"next.zip"));
    FLOAR_CHECK(opener[L"next.zip"].threadId == opener[L"x.zip"].threadId);
    FLOAR_CHECK(cache.WorkerCount() == 1);
    opener.Unblock(L"next.zip");
    FLOAR_REQUIRE(Ready(callerNext));
    FLOAR_CHECK(callerNext.get() != nullptr);
    FLOAR_CHECK(!opener[L"next.zip"].aborted);
    FLOAR_CHECK(cache.Shutdown(5s));
}

FLOAR_TEST(Pool_BlockedOsReadIsCancelledAtShutdown)
{
    ScopedDir dir { TempDir(L"pipe-shutdown") };
    const auto x = dir.File(L"x.zip");
    PipeOpener opener({ L"x.zip" });
    ArchiveReaderCache cache(4, opener.Make(), 1);

    auto caller = GetAsync(cache, x);   // untokened: only Shutdown releases it
    FLOAR_REQUIRE(opener.WaitBlocked(L"x.zip"));
    const bool joined = cache.Shutdown(5s);
    if (!joined)
    {
        opener.Unblock(L"x.zip");
        (void)cache.Shutdown(5s);
    }
    FLOAR_CHECK(joined);
    FLOAR_CHECK(opener[L"x.zip"].aborted);
    FLOAR_REQUIRE(Ready(caller));
    FLOAR_CHECK(caller.get() == nullptr);
    FLOAR_CHECK(cache.WorkerCount() == 0);
}

FLOAR_TEST(Pool_ReplacedArchiveGetsItsOwnOpenAndTheNewestReaderIsCached)
{
    ScopedDir dir { TempDir(L"replace") };
    const auto a = dir.File(L"a.zip");
    GatedOpener opener;
    ArchiveReaderCache cache(4, opener.Make(), 2);

    auto callerOld = GetAsync(cache, a);
    FLOAR_REQUIRE(opener.WaitCalls(1));
    // The file is replaced while the first open still scans the old one.
    std::ofstream(a, std::ios::binary | std::ios::trunc) << "replaced";
    auto callerNew = GetAsync(cache, a);
    // A second open for the new identity, not a join of the old one.
    FLOAR_REQUIRE(opener.WaitCalls(2));
    FLOAR_CHECK(cache.RunningOpenCount() == 2);

    opener.ReleaseCall(1);
    FLOAR_REQUIRE(Ready(callerNew));
    const auto newReader = callerNew.get();
    FLOAR_CHECK(Generation(newReader) == 2);
    FLOAR_CHECK(cache.Get(a) == newReader);

    // The old open was not stopped and still serves its own caller...
    opener.ReleaseCall(0);
    FLOAR_REQUIRE(Ready(callerOld));
    FLOAR_CHECK(Generation(callerOld.get()) == 1);
    FLOAR_CHECK(!opener.Calls()[0].stopSeen);
    // ...but finishing last does not replace the newer cached reader.
    FLOAR_CHECK(cache.Get(a) == newReader);
    FLOAR_CHECK(cache.OpenCount() == 2);
    FLOAR_CHECK(cache.Shutdown(5s));
}
