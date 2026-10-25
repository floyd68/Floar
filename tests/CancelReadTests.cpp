#include "TestHarness.h"

#include "ArchiveReader.h"
#include "ArchiveReaderCache.h"
#include "TestArchives.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// Stop-aware VirtualFileSystem::ReadFile through the injectable cache seam:
// pre-canceled reads of archive entries and plain files do no work, normal
// reads return unchanged bytes, a stop during a cold open or a shared open
// releases only that caller, and a stop during extraction of a generated solid
// 7z (LZMA2) ends at the next reachable check and publishes nothing.

namespace
{
    using namespace Floar;
    using namespace std::chrono_literals;

    std::filesystem::path TempDir(const std::wstring& tag)
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        const std::filesystem::path dir = std::filesystem::path(tempPath) /
            (L"FloarCancel_" + std::to_wstring(GetCurrentProcessId()) + L"_" + tag + L"_" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(dir);
        return dir;
    }

    struct ScopedDir
    {
        std::filesystem::path path;
        ~ScopedDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    std::stop_source Stopped()
    {
        std::stop_source stop;
        stop.request_stop();
        return stop;
    }

    std::vector<uint8_t> Bytes(const std::string& text)
    {
        return std::vector<uint8_t>(text.begin(), text.end());
    }

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

    // Random-access reader with one entry; counts extractions and can request
    // a stop while an extraction runs.
    class CountingReader final : public IArchiveReader
    {
    public:
        const std::vector<ArchiveEntry>& Entries() const override { return m_entries; }
        std::vector<uint8_t> ExtractToMemory(const std::wstring&) override
        {
            ++extractions;
            if (stopDuringExtraction != nullptr)
            {
                stopDuringExtraction->request_stop();
            }
            return Bytes("payload");
        }
        bool HasEntry(const std::wstring&) override { return true; }
        bool HasDirectory(const std::wstring&) const override { return false; }
        std::wstring GetFormatName() const override { return L"counting"; }

        std::atomic<int> extractions { 0 };
        std::stop_source* stopDuringExtraction { nullptr };

    private:
        std::vector<ArchiveEntry> m_entries { { L"e.dds", 7, 7, false, 0 } };
    };

    std::filesystem::path WriteFileText(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        return path;
    }
}

FLOAR_TEST(Cancel_PreCanceledReadsDoNoWork)
{
    ScopedDir dir { TempDir(L"pre") };
    const auto archive = WriteFileText(dir.path / L"a.ba2", "x");
    const auto plain = WriteFileText(dir.path / L"plain.bin", "plain bytes");
    auto reader = std::make_shared<CountingReader>();
    std::atomic<int> opens { 0 };
    ArchiveReaderCache cache(4, [&](const std::filesystem::path&, std::stop_token) -> std::shared_ptr<IArchiveReader>
    {
        ++opens;
        return reader;
    });
    const VirtualPath entry(archive, L"e.dds");

    // Uncached archive: no open, no worker.
    FLOAR_CHECK(VirtualFileSystem::ReadFile(entry, Stopped().get_token(), cache).empty());
    FLOAR_CHECK(opens == 0);
    FLOAR_CHECK(cache.WorkerCount() == 0);
    // Cached archive: no extraction.
    FLOAR_CHECK(VirtualFileSystem::ReadFile(entry, std::stop_token {}, cache) == Bytes("payload"));
    FLOAR_CHECK(reader->extractions == 1);
    FLOAR_CHECK(VirtualFileSystem::ReadFile(entry, Stopped().get_token(), cache).empty());
    FLOAR_CHECK(reader->extractions == 1);
    FLOAR_CHECK(opens == 1);
    // Plain file.
    FLOAR_CHECK(VirtualFileSystem::ReadFile(VirtualPath(plain), Stopped().get_token()).empty());
    FLOAR_CHECK(VirtualFileSystem::ReadFile(VirtualPath(plain), std::stop_token {}) == Bytes("plain bytes"));
    FLOAR_CHECK(cache.Shutdown(5s));
}

FLOAR_TEST(Cancel_StopDuringExtractionPublishesNoData)
{
    ScopedDir dir { TempDir(L"extract") };
    const auto archive = WriteFileText(dir.path / L"a.ba2", "x");
    auto reader = std::make_shared<CountingReader>();
    ArchiveReaderCache cache(4, [&](const std::filesystem::path&, std::stop_token) -> std::shared_ptr<IArchiveReader> { return reader; });
    const VirtualPath entry(archive, L"e.dds");

    std::stop_source stop;
    reader->stopDuringExtraction = &stop;
    FLOAR_CHECK(VirtualFileSystem::ReadFile(entry, stop.get_token(), cache).empty());
    FLOAR_CHECK(reader->extractions == 1);
    // The cached reader still serves other callers.
    reader->stopDuringExtraction = nullptr;
    FLOAR_CHECK(VirtualFileSystem::ReadFile(entry, std::stop_token {}, cache) == Bytes("payload"));
    FLOAR_CHECK(cache.OpenCount() == 1);
    FLOAR_CHECK(cache.Shutdown(5s));
}

FLOAR_TEST(Cancel_ColdOpenAndSharedOpenReleaseOnlyTheStoppedCaller)
{
    ScopedDir dir { TempDir(L"open") };
    const auto archive = WriteFileText(dir.path / L"a.ba2", "x");
    const VirtualPath entry(archive, L"e.dds");

    std::mutex mutex;
    std::condition_variable_any changed;
    int entered = 0;
    int stopsSeen = 0;
    bool release = false;
    ArchiveReaderCache cache(4, [&](const std::filesystem::path&, std::stop_token stop) -> std::shared_ptr<IArchiveReader>
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++entered;
        changed.notify_all();
        if (!changed.wait(lock, stop, [&] { return release; }))
        {
            ++stopsSeen;
            return nullptr;
        }
        return std::make_shared<CountingReader>();
    });
    const auto waitEntered = [&](int count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return entered >= count; });
    };

    // Cold open with a single caller: its stop ends the scan too.
    std::stop_source stopCold;
    auto cold = std::async(std::launch::async, [&] { return VirtualFileSystem::ReadFile(entry, stopCold.get_token(), cache); });
    FLOAR_REQUIRE(waitEntered(1));
    stopCold.request_stop();
    FLOAR_REQUIRE(Ready(cold));
    FLOAR_CHECK(cold.get().empty());
    FLOAR_CHECK(WaitUntil([&] { std::lock_guard<std::mutex> lock(mutex); return stopsSeen == 1; }));

    // Shared open: A initiates and cancels, B still gets the bytes from the same open.
    std::stop_source stopA;
    auto readA = std::async(std::launch::async, [&] { return VirtualFileSystem::ReadFile(entry, stopA.get_token(), cache); });
    FLOAR_REQUIRE(waitEntered(2));
    std::stop_source stopB;
    auto readB = std::async(std::launch::async, [&] { return VirtualFileSystem::ReadFile(entry, stopB.get_token(), cache); });
    FLOAR_REQUIRE(WaitUntil([&] { return cache.WaitingCount() == 2; }));
    stopA.request_stop();
    FLOAR_REQUIRE(Ready(readA));
    FLOAR_CHECK(readA.get().empty());
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        changed.notify_all();
    }
    FLOAR_REQUIRE(Ready(readB));
    FLOAR_CHECK(readB.get() == Bytes("payload"));
    FLOAR_CHECK(cache.OpenCount() == 2);
    {
        std::lock_guard<std::mutex> lock(mutex);
        FLOAR_CHECK(stopsSeen == 1);   // only the cold open was stopped
    }
    FLOAR_CHECK(cache.Shutdown(5s));
}

#if FLOAR_ENABLE_COMMON_ARCHIVES
namespace
{
    constexpr int kSolidEntries = 48;
    constexpr size_t kSolidEntryBytes = 512 * 1024;

    std::wstring SolidName(int i)
    {
        wchar_t buffer[32] {};
        swprintf_s(buffer, L"img%03d.bin", i);
        return buffer;
    }

    // Compressible pseudo-random data, so LZMA2 has real decoding work.
    std::string SolidData(int i)
    {
        std::mt19937 random(static_cast<unsigned>(i) + 1);
        std::string data(kSolidEntryBytes, '\0');
        for (char& c : data)
        {
            c = static_cast<char>('a' + (random() % 16));
        }
        return data;
    }

    // One generated solid 7z per test run.
    struct SolidFixture
    {
        ScopedDir dir { TempDir(L"solid") };
        std::filesystem::path archive = dir.path / L"solid.7z";
        bool written = false;

        SolidFixture()
        {
            std::vector<std::pair<std::wstring, std::string>> files;
            for (int i = 0; i < kSolidEntries; ++i)
            {
                files.emplace_back(SolidName(i), SolidData(i));
            }
            written = FloarTest::WriteSolid7z(archive, files, 0);
        }
    };

    SolidFixture& Solid()
    {
        static SolidFixture* fixture = []
        {
            auto* created = new SolidFixture();
            FloarTest::RegisterTeardown([created] { delete created; });
            return created;
        }();
        return *fixture;
    }

    std::vector<uint8_t> AsBytes(const std::string& text)
    {
        return std::vector<uint8_t>(text.begin(), text.end());
    }
}

FLOAR_TEST(Cancel_SolidReadsReturnUnchangedBytes)
{
    SolidFixture& solid = Solid();
    FLOAR_REQUIRE(solid.written);
    ArchiveReaderCache cache(4);
    const int last = kSolidEntries - 1;
    const VirtualPath first(solid.archive, SolidName(0));
    const VirtualPath lastEntry(solid.archive, SolidName(last));
    std::stop_source live;
    FLOAR_CHECK(VirtualFileSystem::ReadFile(first, live.get_token(), cache) == AsBytes(SolidData(0)));
    FLOAR_CHECK(VirtualFileSystem::ReadFile(lastEntry, std::stop_token {}, cache) == AsBytes(SolidData(last)));
    // The legacy overload (the shared cache) returns the same bytes.
    FLOAR_CHECK(VirtualFileSystem::ReadFile(lastEntry) == AsBytes(SolidData(last)));
    FLOAR_CHECK(cache.OpenCount() == 1);
    FLOAR_CHECK(cache.Shutdown(5s));
}

FLOAR_TEST(Cancel_SolidBatchStopSkipsTheRemainingEntries)
{
    SolidFixture& solid = Solid();
    FLOAR_REQUIRE(solid.written);
    ArchiveReaderCache cache(4);
    const auto reader = cache.Get(solid.archive);
    FLOAR_REQUIRE(reader != nullptr);

    std::vector<std::wstring> all;
    for (int i = 0; i < kSolidEntries; ++i)
    {
        all.push_back(SolidName(i));
    }
    constexpr int kStopAfter = 3;
    std::stop_source stop;
    int delivered = 0;
    const size_t headersBefore = ArchiveReaderFactory::ExtractionHeaderCountForTesting();
    FLOAR_CHECK(!reader->ExtractBatch(all, stop.get_token(), [&](const std::wstring&, std::vector<uint8_t>&&)
    {
        if (++delivered == kStopAfter)
        {
            stop.request_stop();
        }
    }));
    FLOAR_CHECK(delivered == kStopAfter);
    // The scan read no header after the stop.
    FLOAR_CHECK(ArchiveReaderFactory::ExtractionHeaderCountForTesting() - headersBefore == kStopAfter);
    FLOAR_CHECK(cache.Shutdown(5s));
}

FLOAR_TEST(Cancel_SolidReadStoppedMidwayEndsAtTheNextCheck)
{
    SolidFixture& solid = Solid();
    FLOAR_REQUIRE(solid.written);
    ArchiveReaderCache cache(4);
    const int last = kSolidEntries - 1;
    const VirtualPath lastEntry(solid.archive, SolidName(last));

    // Real cost of reaching the last entry (open included, then a warm read).
    FLOAR_REQUIRE(VirtualFileSystem::ReadFile(lastEntry, std::stop_token {}, cache) == AsBytes(SolidData(last)));
    const auto fullStart = std::chrono::steady_clock::now();
    FLOAR_REQUIRE(VirtualFileSystem::ReadFile(lastEntry, std::stop_token {}, cache) == AsBytes(SolidData(last)));
    const auto fullRead = std::chrono::steady_clock::now() - fullStart;

    // libarchive's 7z reader reads every header up front and decompresses the
    // entries before the requested one inside the first data read, so progress
    // is measured in archive bytes read rather than in entries.
    const size_t archiveBytes = static_cast<size_t>(std::filesystem::file_size(solid.archive));
    std::stop_source stop;
    const size_t bytesBefore = ArchiveReaderFactory::LibArchiveBytesReadForTesting();
    const auto bytesRead = [&] { return ArchiveReaderFactory::LibArchiveBytesReadForTesting() - bytesBefore; };
    auto read = std::async(std::launch::async, [&] { return VirtualFileSystem::ReadFile(lastEntry, stop.get_token(), cache); });
    // Stop once an eighth of the archive was decompressed.
    const bool underway = WaitUntil([&] { return bytesRead() >= archiveBytes / 8; }, 10s);
    const size_t bytesAtStop = bytesRead();
    const auto stopAt = std::chrono::steady_clock::now();
    stop.request_stop();
    FLOAR_REQUIRE(Ready(read, 30s));
    const auto latency = std::chrono::steady_clock::now() - stopAt;
    const size_t bytesAtReturn = bytesRead();
    const auto data = read.get();

    std::printf("  solid 7z (%zu bytes): full read of entry %d %lld ms; stop after %zu bytes, latency %lld us, %zu bytes read after the stop\n",
        archiveBytes, last,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(fullRead).count()),
        bytesAtStop,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(latency).count()),
        bytesAtReturn - bytesAtStop);
    FLOAR_REQUIRE(underway);
    FLOAR_CHECK(data.empty());
    // At most the block being read when stop arrived (64 KiB) completes, and
    // the rest of the archive is not processed.
    FLOAR_CHECK(bytesAtReturn - bytesAtStop <= 64 * 1024);
    FLOAR_CHECK(bytesAtReturn < archiveBytes);
    FLOAR_CHECK(latency < fullRead);
    // The cached reader still serves other callers.
    FLOAR_CHECK(VirtualFileSystem::ReadFile(VirtualPath(solid.archive, SolidName(1)), std::stop_token {}, cache) == AsBytes(SolidData(1)));
    FLOAR_CHECK(cache.OpenCount() == 1);
    FLOAR_CHECK(cache.Shutdown(5s));
}
#endif
