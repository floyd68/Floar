#include "TestHarness.h"

#include "ArchiveCursorPool.h"
#include "ArchiveReader.h"
#include "ArchiveReaderCache.h"
#include "TestArchives.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <map>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// Forward cursors over generated solid 7z archives (libarchive's 7-Zip
// writer, LZMA2): successive archive-order extractions continue one cursor
// (no reopen, no replayed headers), backward requests restart, cursor states
// after success, missing entries, the end of the archive, errors, stops and
// sink-ended passes, cancelable admission, High priority past Normal cursors,
// replaced archives, correct bytes under concurrent extraction, and ZIP/BA2
// readers that use no cursor. Work is measured with deterministic counters
// (archive handle opens, entry headers read, pool statistics), never with
// compressed-byte counts.

#if FLOAR_ENABLE_COMMON_ARCHIVES
namespace
{
    using namespace Floar;
    using namespace std::chrono_literals;

    std::filesystem::path TempDir(const std::wstring& tag)
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        const std::filesystem::path dir = std::filesystem::path(tempPath) /
            (L"FloarCursor_" + std::to_wstring(GetCurrentProcessId()) + L"_" + tag + L"_" + std::to_wstring(GetTickCount64()));
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

    // A solid 7z: 8 entries in another folder, then 24 in a deep folder; every
    // entry differs and compresses (16 symbols), so reading one decompresses
    // all before it.
    constexpr int kOther = 8;
    constexpr int kDeep = 24;
    constexpr int kEntries = kOther + kDeep;

    std::wstring EntryName(int i)
    {
        wchar_t buffer[64] {};
        if (i < kOther)
        {
            swprintf_s(buffer, L"Data/Other/o%02d.bin", i);
        }
        else
        {
            swprintf_s(buffer, L"Data/Textures/Deep/Folder/t%02d.dds", i - kOther);
        }
        return buffer;
    }

    std::string EntryData(int i, char tag = 'a', size_t size = 16 * 1024)
    {
        std::mt19937 random(static_cast<unsigned>(i) * 13 + static_cast<unsigned>(tag));
        std::string data(size, '\0');
        for (char& c : data)
        {
            c = static_cast<char>(tag + (random() % 16));
        }
        return data;
    }

    std::vector<uint8_t> Bytes(const std::string& text)
    {
        return std::vector<uint8_t>(text.begin(), text.end());
    }

    bool WriteFixture(const std::filesystem::path& path, char tag = 'a', size_t size = 16 * 1024)
    {
        std::vector<std::pair<std::wstring, std::string>> files;
        for (int i = 0; i < kEntries; ++i)
        {
            files.emplace_back(EntryName(i), EntryData(i, tag, size));
        }
        return FloarTest::WriteSolid7z(path, files, 1);
    }

    struct Fixture
    {
        ScopedDir dir;
        std::filesystem::path archive = dir.path / L"solid.7z";
        bool written = WriteFixture(archive);

        explicit Fixture(const std::wstring& tag)
            : dir { TempDir(tag) }
        {
        }
    };

    std::shared_ptr<ArchiveCursorPool> NewPool(ArchiveCursorLimits limits = {})
    {
        return std::make_shared<ArchiveCursorPool>(limits);
    }

    struct Counters
    {
        size_t handles = ArchiveReaderFactory::DataHandleOpenCountForTesting();
        size_t headers = ArchiveReaderFactory::ExtractionHeaderCountForTesting();

        size_t Handles() const { return ArchiveReaderFactory::DataHandleOpenCountForTesting() - handles; }
        size_t Headers() const { return ArchiveReaderFactory::ExtractionHeaderCountForTesting() - headers; }
    };

    // ExtractForward collecting every delivery.
    struct Collected
    {
        IArchiveReader::ExtractResult result {};
        std::map<std::wstring, std::vector<uint8_t>> data;
        std::vector<std::wstring> order;
    };

    Collected Forward(IArchiveReader& reader, const std::vector<std::wstring>& paths, std::stop_token stop = {},
        ExtractPriority priority = ExtractPriority::Normal, size_t endAfter = static_cast<size_t>(-1))
    {
        Collected collected;
        IArchiveReader::ExtractOptions options;
        options.priority = priority;
        collected.result = reader.ExtractForward(paths, stop, [&](const std::wstring& path, std::vector<uint8_t>&& data)
        {
            collected.order.push_back(path);
            collected.data[path] = std::move(data);
            return collected.order.size() < endAfter;
        }, options);
        return collected;
    }
}

// The key behaviour, with the APIs master already had: on master every
// ExtractToMemory opens the archive and reads every header up to its entry
// (kEntries opens and kEntries*(kEntries+1)/2 headers here).
FLOAR_TEST(CursorRead_ArchiveOrderContinuesWithoutReopenOrReplay)
{
    Fixture fixture(L"order");
    FLOAR_REQUIRE(fixture.written);
    const auto reader = ArchiveReaderFactory::Open(fixture.archive);
    FLOAR_REQUIRE(reader != nullptr);

    const Counters counters;
    for (int i = 0; i < kEntries; ++i)
    {
        FLOAR_CHECK(reader->ExtractToMemory(EntryName(i)) == Bytes(EntryData(i)));
    }
    std::printf("  %d archive-order extractions: %zu archive opens, %zu headers read\n", kEntries, counters.Handles(), counters.Headers());
    FLOAR_CHECK(counters.Handles() == 1);
    FLOAR_CHECK(counters.Headers() == static_cast<size_t>(kEntries));
}

FLOAR_TEST(CursorRead_DeepFolderThroughTheVfsResumesOneCursor)
{
    Fixture fixture(L"vfs");
    FLOAR_REQUIRE(fixture.written);
    const auto pool = NewPool();
    ArchiveReaderCache cache(4, [pool](const std::filesystem::path& path, std::stop_token stop)
    {
        return std::shared_ptr<IArchiveReader>(ArchiveReaderFactory::Open(path, stop, pool));
    });

    const Counters counters;
    for (int i = kOther; i < kEntries; ++i)
    {
        const auto data = VirtualFileSystem::ReadFile(VirtualPath(fixture.archive, EntryName(i)), std::stop_token {}, cache);
        FLOAR_CHECK(data == Bytes(EntryData(i)));
    }
    const auto stats = pool->Stats();
    FLOAR_CHECK(stats.opens == 1);
    FLOAR_CHECK(stats.resumes == static_cast<uint64_t>(kDeep - 1));
    FLOAR_CHECK(stats.restarts == 0);
    // The first read skips the other folder once; later ones skip nothing.
    FLOAR_CHECK(stats.headersRead == static_cast<uint64_t>(kEntries));
    FLOAR_CHECK(stats.entriesRead == static_cast<uint64_t>(kDeep));
    FLOAR_CHECK(counters.Handles() == 1);
    // The last entry was read: nothing left to continue, so it was closed.
    FLOAR_CHECK(stats.idle == 0);
    FLOAR_CHECK(cache.Shutdown(5s));
}

FLOAR_TEST(CursorRead_CapabilityAndArchiveOrder)
{
    Fixture fixture(L"order2");
    FLOAR_REQUIRE(fixture.written);
    const auto reader = ArchiveReaderFactory::Open(fixture.archive, {}, NewPool());
    FLOAR_REQUIRE(reader != nullptr);
    FLOAR_CHECK(reader->HasExpensiveRandomAccess());
    for (int i = 0; i < kEntries; ++i)
    {
        FLOAR_CHECK(reader->ArchiveOrder(EntryName(i)) == static_cast<size_t>(i));
    }
    // Any spelling of a path; directories and missing entries have no order.
    FLOAR_CHECK(reader->ArchiveOrder(L"DATA\\other\\O03.BIN") == 3);
    FLOAR_CHECK(reader->ArchiveOrder(L"Data/Other") == IArchiveReader::kNoArchiveOrder);
    FLOAR_CHECK(reader->ArchiveOrder(L"missing.bin") == IArchiveReader::kNoArchiveOrder);
}

FLOAR_TEST(CursorRead_BackwardRequestRestartsAndKeepsTheForwardCursor)
{
    Fixture fixture(L"back");
    FLOAR_REQUIRE(fixture.written);
    const auto pool = NewPool();
    const auto reader = ArchiveReaderFactory::Open(fixture.archive, {}, pool);
    FLOAR_REQUIRE(reader != nullptr);

    const Counters counters;
    FLOAR_CHECK(reader->ExtractToMemory(EntryName(10)) == Bytes(EntryData(10)));
    FLOAR_CHECK(counters.Headers() == 11);
    // Behind the cursor (at 11): a restart from the first header.
    FLOAR_CHECK(reader->ExtractToMemory(EntryName(3)) == Bytes(EntryData(3)));
    FLOAR_CHECK(counters.Handles() == 2);
    FLOAR_CHECK(counters.Headers() == 11 + 4);
    auto stats = pool->Stats();
    FLOAR_CHECK(stats.restarts == 1);
    FLOAR_CHECK(stats.idle == 2);   // at 4 and at 11

    // Forward again: the cursor at 11 continues (one header), not the one at 4.
    FLOAR_CHECK(reader->ExtractToMemory(EntryName(11)) == Bytes(EntryData(11)));
    FLOAR_CHECK(counters.Handles() == 2);
    FLOAR_CHECK(counters.Headers() == 11 + 4 + 1);
    // And the one at 4 serves a request between them.
    FLOAR_CHECK(reader->ExtractToMemory(EntryName(6)) == Bytes(EntryData(6)));
    FLOAR_CHECK(counters.Handles() == 2);
    FLOAR_CHECK(counters.Headers() == 11 + 4 + 1 + 3);
    stats = pool->Stats();
    FLOAR_CHECK(stats.opens == 2);
    FLOAR_CHECK(stats.resumes == 2);
}

FLOAR_TEST(CursorRead_MissingEntriesAndDirectoriesLeaveTheCursorInPlace)
{
    Fixture fixture(L"missing");
    FLOAR_REQUIRE(fixture.written);
    const auto pool = NewPool();
    const auto reader = ArchiveReaderFactory::Open(fixture.archive, {}, pool);
    FLOAR_REQUIRE(reader != nullptr);

    FLOAR_CHECK(reader->ExtractToMemory(EntryName(2)) == Bytes(EntryData(2)));
    const Counters counters;
    FLOAR_CHECK(reader->ExtractToMemory(L"Data/missing.bin").empty());
    FLOAR_CHECK(reader->ExtractToMemory(L"Data/Other").empty());
    FLOAR_CHECK(counters.Headers() == 0);
    FLOAR_CHECK(counters.Handles() == 0);
    FLOAR_CHECK(pool->Stats().active == 0);

    // Missing entries in a batch are delivered (empty) after the others.
    const auto collected = Forward(*reader, { L"Data/missing.bin", EntryName(5) });
    FLOAR_CHECK(collected.result == IArchiveReader::ExtractResult::Completed);
    FLOAR_CHECK(collected.order == (std::vector<std::wstring> { EntryName(5), L"Data/missing.bin" }));
    FLOAR_CHECK(collected.data.at(EntryName(5)) == Bytes(EntryData(5)));
    FLOAR_CHECK(collected.data.at(L"Data/missing.bin").empty());
    FLOAR_CHECK(counters.Handles() == 0);
    FLOAR_CHECK(counters.Headers() == 3);
    FLOAR_CHECK(pool->Stats().idle == 1);
    FLOAR_CHECK(pool->Stats().resumes == 1);
}

FLOAR_TEST(CursorRead_EndOfArchiveClosesTheCursor)
{
    Fixture fixture(L"eof");
    FLOAR_REQUIRE(fixture.written);
    const auto pool = NewPool();
    const auto reader = ArchiveReaderFactory::Open(fixture.archive, {}, pool);
    FLOAR_REQUIRE(reader != nullptr);

    FLOAR_CHECK(reader->ExtractToMemory(EntryName(kEntries - 2)) == Bytes(EntryData(kEntries - 2)));
    FLOAR_CHECK(pool->Stats().idle == 1);
    FLOAR_CHECK(reader->ExtractToMemory(EntryName(kEntries - 1)) == Bytes(EntryData(kEntries - 1)));
    const auto stats = pool->Stats();
    FLOAR_CHECK(stats.resumes == 1);
    FLOAR_CHECK(stats.idle == 0);
    FLOAR_CHECK(stats.discarded == 1);
    FLOAR_CHECK(stats.memoryBytes == 0);
}

FLOAR_TEST(CursorRead_SinkEndKeepsTheCursorAfterThatEntry)
{
    Fixture fixture(L"sink");
    FLOAR_REQUIRE(fixture.written);
    const auto pool = NewPool();
    const auto reader = ArchiveReaderFactory::Open(fixture.archive, {}, pool);
    FLOAR_REQUIRE(reader != nullptr);

    const Counters counters;
    // Request order does not matter; delivery is in archive order.
    const auto collected = Forward(*reader, { EntryName(6), EntryName(3), EntryName(5), EntryName(4), L"missing" }, {},
        ExtractPriority::Normal, 2);
    FLOAR_CHECK(collected.result == IArchiveReader::ExtractResult::EndedBySink);
    FLOAR_CHECK(collected.order == (std::vector<std::wstring> { EntryName(3), EntryName(4) }));
    FLOAR_CHECK(collected.data.at(EntryName(4)) == Bytes(EntryData(4)));
    FLOAR_CHECK(counters.Headers() == 5);
    FLOAR_CHECK(pool->Stats().idle == 1);

    const auto next = Forward(*reader, { EntryName(5), EntryName(6) });
    FLOAR_CHECK(next.result == IArchiveReader::ExtractResult::Completed);
    FLOAR_CHECK(next.data.at(EntryName(6)) == Bytes(EntryData(6)));
    FLOAR_CHECK(counters.Handles() == 1);
    FLOAR_CHECK(counters.Headers() == 7);
}

FLOAR_TEST(CursorRead_StopTransitions)
{
    Fixture fixture(L"stop");
    FLOAR_REQUIRE(fixture.written);
    const auto pool = NewPool();
    const auto reader = ArchiveReaderFactory::Open(fixture.archive, {}, pool);
    FLOAR_REQUIRE(reader != nullptr);
    FLOAR_CHECK(reader->ExtractToMemory(EntryName(1)) == Bytes(EntryData(1)));
    const Counters counters;

    // Stopped before the call: no work, the retained cursor untouched.
    std::stop_source before;
    before.request_stop();
    auto collected = Forward(*reader, { EntryName(2) }, before.get_token());
    FLOAR_CHECK(collected.result == IArchiveReader::ExtractResult::Stopped);
    FLOAR_CHECK(collected.order.empty());
    FLOAR_CHECK(counters.Headers() == 0);
    FLOAR_CHECK(pool->Stats().idle == 1);
    FLOAR_CHECK(pool->Stats().resumes == 0);

    // Stopped from a delivery, between entries: the cursor stays after the
    // delivered entry and the next request continues it.
    std::stop_source fromSink;
    int deliveries = 0;
    const auto result = reader->ExtractForward({ EntryName(2), EntryName(3), EntryName(4) }, fromSink.get_token(),
        [&](const std::wstring&, std::vector<uint8_t>&&)
        {
            ++deliveries;
            fromSink.request_stop();
            return true;
        }, {});
    FLOAR_CHECK(result == IArchiveReader::ExtractResult::Stopped);
    FLOAR_CHECK(deliveries == 1);
    FLOAR_CHECK(counters.Headers() == 1);
    FLOAR_CHECK(pool->Stats().idle == 1);
    FLOAR_CHECK(reader->ExtractToMemory(EntryName(3)) == Bytes(EntryData(3)));
    FLOAR_CHECK(counters.Handles() == 0);
    FLOAR_CHECK(counters.Headers() == 2);

    // Stopped inside libarchive (a block read on the way to a later entry):
    // the handle has no known position and is closed.
    std::stop_source inside;
    ArchiveReaderFactory::SetLibArchiveReadHookForTesting([&](uint64_t)
    {
        inside.request_stop();
        return false;
    });
    collected = Forward(*reader, { EntryName(kEntries - 3) }, inside.get_token());
    ArchiveReaderFactory::SetLibArchiveReadHookForTesting({});
    FLOAR_CHECK(collected.result == IArchiveReader::ExtractResult::Stopped);
    FLOAR_CHECK(collected.order.empty());
    auto stats = pool->Stats();
    FLOAR_CHECK(stats.idle == 0);
    FLOAR_CHECK(stats.discarded == 1);
    FLOAR_CHECK(stats.active == 0);
    // The next request restarts and reads the right bytes.
    FLOAR_CHECK(reader->ExtractToMemory(EntryName(kEntries - 3)) == Bytes(EntryData(kEntries - 3)));
    FLOAR_CHECK(counters.Handles() == 1);
}

FLOAR_TEST(CursorRead_ReadErrorDiscardsTheCursor)
{
    // Incompressible 64 KiB entries: LZMA2 stores them, so archive offsets
    // follow entry order and a failing offset is reached by a known entry.
    ScopedDir dir { TempDir(L"error") };
    const auto archive = dir.path / L"stored.7z";
    std::vector<std::pair<std::wstring, std::string>> files;
    std::mt19937 random(3);
    std::vector<std::string> contents;
    for (int i = 0; i < 16; ++i)
    {
        std::string data(64 * 1024, '\0');
        for (char& c : data)
        {
            c = static_cast<char>(random());
        }
        contents.push_back(data);
        files.emplace_back(L"e" + std::to_wstring(100 + i), data);
    }
    FLOAR_REQUIRE(FloarTest::WriteSolid7z(archive, files, 1));
    const auto pool = NewPool();
    const auto reader = ArchiveReaderFactory::Open(archive, {}, pool);
    FLOAR_REQUIRE(reader != nullptr);

    FLOAR_CHECK(reader->ExtractToMemory(L"e100") == Bytes(contents[0]));
    FLOAR_CHECK(pool->Stats().idle == 1);
    ArchiveReaderFactory::SetLibArchiveReadHookForTesting([](uint64_t offset) { return offset >= 8 * 64 * 1024; });
    FLOAR_CHECK(reader->ExtractToMemory(L"e112").empty());
    ArchiveReaderFactory::SetLibArchiveReadHookForTesting({});
    auto stats = pool->Stats();
    FLOAR_CHECK(stats.resumes == 1);
    FLOAR_CHECK(stats.idle == 0);
    FLOAR_CHECK(stats.discarded == 1);

    // Recovered with a new cursor.
    const Counters counters;
    FLOAR_CHECK(reader->ExtractToMemory(L"e112") == Bytes(contents[12]));
    FLOAR_CHECK(counters.Handles() == 1);
    FLOAR_CHECK(pool->Stats().idle == 1);
}

FLOAR_TEST(CursorRead_CanceledAdmissionLeaksNothing)
{
    Fixture fixture(L"admit");
    FLOAR_REQUIRE(fixture.written);
    ArchiveCursorLimits limits;
    limits.maxActive = 1;
    limits.maxActivePerReader = 1;
    limits.reservedForHigh = 0;
    const auto pool = NewPool(limits);
    const auto reader = ArchiveReaderFactory::Open(fixture.archive, {}, pool);
    FLOAR_REQUIRE(reader != nullptr);

    // A holds the only cursor inside its delivery.
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    auto blocking = std::async(std::launch::async, [&]
    {
        return reader->ExtractForward({ EntryName(0) }, {}, [&](const std::wstring&, std::vector<uint8_t>&&)
        {
            releaseFuture.wait();
            return true;
        }, {});
    });
    FLOAR_REQUIRE(WaitUntil([&] { return pool->Stats().active == 1; }));

    // B waits for capacity and is stopped: it returns without delivering.
    std::stop_source stop;
    int deliveries = 0;
    auto waiter = std::async(std::launch::async, [&]
    {
        return reader->ExtractForward({ EntryName(5) }, stop.get_token(), [&](const std::wstring&, std::vector<uint8_t>&&)
        {
            ++deliveries;
            return true;
        }, {});
    });
    FLOAR_REQUIRE(WaitUntil([&] { return pool->Stats().waiting == 1; }));
    const auto stopAt = std::chrono::steady_clock::now();
    stop.request_stop();
    FLOAR_REQUIRE(waiter.wait_for(10s) == std::future_status::ready);
    const auto latency = std::chrono::steady_clock::now() - stopAt;
    FLOAR_CHECK(waiter.get() == IArchiveReader::ExtractResult::Stopped);
    FLOAR_CHECK(deliveries == 0);
    auto stats = pool->Stats();
    FLOAR_CHECK(stats.canceledWaits == 1);
    FLOAR_CHECK(stats.waiting == 0);
    FLOAR_CHECK(stats.active == 1);
    std::printf("  canceled admission returned in %lld us\n",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(latency).count()));

    release.set_value();
    FLOAR_REQUIRE(blocking.wait_for(10s) == std::future_status::ready);
    FLOAR_CHECK(blocking.get() == IArchiveReader::ExtractResult::Completed);
    FLOAR_CHECK(pool->Stats().active == 0);
    // Capacity was not leaked: the next extraction is admitted.
    FLOAR_CHECK(reader->ExtractToMemory(EntryName(5)) == Bytes(EntryData(5)));
}

FLOAR_TEST(CursorRead_HighPriorityIsNotBlockedByNormalCursors)
{
    Fixture fixture(L"high");
    FLOAR_REQUIRE(fixture.written);
    ArchiveCursorLimits limits;
    limits.maxActive = 2;
    limits.maxActivePerReader = 2;
    limits.reservedForHigh = 1;   // Normal: one cursor at a time
    const auto pool = NewPool(limits);
    const auto reader = ArchiveReaderFactory::Open(fixture.archive, {}, pool);
    FLOAR_REQUIRE(reader != nullptr);

    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    auto thumbnail = std::async(std::launch::async, [&]
    {
        return reader->ExtractForward({ EntryName(kOther) }, {}, [&](const std::wstring&, std::vector<uint8_t>&&)
        {
            releaseFuture.wait();
            return true;
        }, {});
    });
    FLOAR_REQUIRE(WaitUntil([&] { return pool->Stats().active == 1; }));
    auto nextThumbnail = std::async(std::launch::async, [&] { return reader->ExtractToMemory(EntryName(kOther + 1)); });
    FLOAR_REQUIRE(WaitUntil([&] { return pool->Stats().waiting == 1; }));

    // The main image of the same archive is read while both thumbnails wait.
    const auto main = Forward(*reader, { EntryName(3) }, {}, ExtractPriority::High);
    FLOAR_CHECK(main.result == IArchiveReader::ExtractResult::Completed);
    FLOAR_CHECK(main.data.at(EntryName(3)) == Bytes(EntryData(3)));
    // The same through the VFS overload with options.
    ArchiveReaderCache cache(4, [pool](const std::filesystem::path& path, std::stop_token stop)
    {
        return std::shared_ptr<IArchiveReader>(ArchiveReaderFactory::Open(path, stop, pool));
    });
    IArchiveReader::ExtractOptions high;
    high.priority = ExtractPriority::High;
    FLOAR_CHECK(VirtualFileSystem::ReadFile(VirtualPath(fixture.archive, EntryName(4)), {}, cache, high) == Bytes(EntryData(4)));
    FLOAR_CHECK(nextThumbnail.wait_for(0ms) == std::future_status::timeout);

    release.set_value();
    FLOAR_REQUIRE(thumbnail.wait_for(10s) == std::future_status::ready);
    FLOAR_REQUIRE(nextThumbnail.wait_for(10s) == std::future_status::ready);
    FLOAR_CHECK(nextThumbnail.get() == Bytes(EntryData(kOther + 1)));
    FLOAR_CHECK(pool->Stats().active == 0);
    FLOAR_CHECK(cache.Shutdown(5s));
}

namespace
{
    // Rewrites `target` in place with the bytes of `source` and a later write
    // time. A retained cursor's handle then reads the new bytes, so only the
    // identity check keeps them from a reader that scanned the old ones.
    // (Renaming over an archive that a cursor holds open fails with
    // ERROR_ACCESS_DENIED, as for any file open without POSIX rename.)
    bool OverwriteInPlace(const std::filesystem::path& source, const std::filesystem::path& target)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(source, ec);
        std::vector<char> bytes(static_cast<size_t>(size));
        FILE* in = nullptr;
        if (ec || _wfopen_s(&in, source.c_str(), L"rb") != 0 || !in)
        {
            return false;
        }
        const bool readAll = fread(bytes.data(), 1, bytes.size(), in) == bytes.size();
        fclose(in);
        const HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (!readAll || file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD written = 0;
        bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
        FILETIME now {};
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER later {};
        later.LowPart = now.dwLowDateTime;
        later.HighPart = now.dwHighDateTime;
        later.QuadPart += 36000000000ull;   // one hour
        const FILETIME writeTime { later.LowPart, later.HighPart };
        ok = SetFileTime(file, nullptr, nullptr, &writeTime) && ok;
        CloseHandle(file);
        return ok;
    }
}

FLOAR_TEST(CursorRead_ReplacedArchiveIsNeverServedFromOldCursors)
{
    Fixture fixture(L"replace");
    FLOAR_REQUIRE(fixture.written);
    const auto replacement = fixture.dir.path / L"replacement.7z";
    FLOAR_REQUIRE(WriteFixture(replacement, 'A'));
    const auto pool = NewPool();

    // A reader with a retained cursor, whose handle would read the new bytes.
    {
        const auto reader = ArchiveReaderFactory::Open(fixture.archive, {}, pool);
        FLOAR_REQUIRE(reader != nullptr);
        FLOAR_CHECK(reader->ExtractToMemory(EntryName(0)) == Bytes(EntryData(0)));
        FLOAR_REQUIRE(pool->Stats().idle == 1);
        FLOAR_REQUIRE(OverwriteInPlace(replacement, fixture.archive));
        // Neither the retained cursor nor a new handle serves the new file
        // to the reader that scanned the old one.
        FLOAR_CHECK(reader->ExtractToMemory(EntryName(1)).empty());
        const auto stats = pool->Stats();
        FLOAR_CHECK(stats.invalidated == 2);   // the retained cursor, then the reopened file
        FLOAR_CHECK(stats.idle == 0);
    }
}

FLOAR_TEST(CursorRead_ConcurrentExtractionReturnsCorrectBytes)
{
    Fixture first(L"conc1");
    FLOAR_REQUIRE(first.written);
    const auto secondPath = first.dir.path / L"second.7z";
    FLOAR_REQUIRE(WriteFixture(secondPath, 'k'));
    ArchiveCursorLimits limits;
    limits.maxActive = 3;
    limits.maxActivePerReader = 2;
    limits.reservedForHigh = 1;
    limits.maxIdle = 2;
    limits.maxIdlePerReader = 2;
    const auto pool = NewPool(limits);
    const std::vector<std::shared_ptr<IArchiveReader>> readers = {
        ArchiveReaderFactory::Open(first.archive, {}, pool),
        ArchiveReaderFactory::Open(secondPath, {}, pool),
    };
    const char tags[] = { 'a', 'k' };
    FLOAR_REQUIRE(readers[0] && readers[1]);

    std::atomic<int> wrong { 0 };
    std::atomic<int> delivered { 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
    {
        threads.emplace_back([&, t]
        {
            std::mt19937 random(static_cast<unsigned>(t) * 31 + 1);
            for (int op = 0; op < 25; ++op)
            {
                const size_t which = random() % 2;
                const int start = static_cast<int>(random() % kEntries);
                const int count = 1 + static_cast<int>(random() % 4);
                std::vector<std::wstring> paths;
                for (int i = start; i < (std::min)(kEntries, start + count); ++i)
                {
                    paths.push_back(EntryName(i));
                }
                const auto priority = (random() % 4 == 0) ? ExtractPriority::High : ExtractPriority::Normal;
                const auto collected = Forward(*readers[which], paths, {}, priority);
                if (collected.result != IArchiveReader::ExtractResult::Completed || collected.data.size() != paths.size())
                {
                    ++wrong;
                }
                for (const auto& [path, data] : collected.data)
                {
                    const int index = static_cast<int>(readers[which]->ArchiveOrder(path));
                    ++delivered;
                    if (data != Bytes(EntryData(index, tags[which])))
                    {
                        ++wrong;
                    }
                }
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    const auto stats = pool->Stats();
    std::printf("  %d entries delivered; opens %llu, resumes %llu, restarts %llu, waits %llu, peak active %zu, peak idle %zu\n",
        delivered.load(), static_cast<unsigned long long>(stats.opens), static_cast<unsigned long long>(stats.resumes),
        static_cast<unsigned long long>(stats.restarts), static_cast<unsigned long long>(stats.waits), stats.peakActive, stats.peakIdle);
    FLOAR_CHECK(wrong == 0);
    FLOAR_CHECK(stats.active == 0);
    FLOAR_CHECK(stats.waiting == 0);
    FLOAR_CHECK(stats.peakActive <= 3);
    FLOAR_CHECK(stats.peakIdle <= 2);
    FLOAR_CHECK(stats.resumes > 0);
}

FLOAR_TEST(CursorRead_ZipKeepsOneScanPerCallAndNoCursor)
{
    ScopedDir dir { TempDir(L"zip") };
    const auto zip = dir.path / L"plain.zip";
    std::vector<std::pair<std::wstring, std::string>> files;
    for (int i = 0; i < 6; ++i)
    {
        files.emplace_back(EntryName(i), EntryData(i));
    }
    FLOAR_REQUIRE(FloarTest::WriteZip(zip, files, {}));
    const auto pool = NewPool();
    const auto reader = ArchiveReaderFactory::Open(zip, {}, pool);
    FLOAR_REQUIRE(reader != nullptr);
    FLOAR_CHECK(!reader->HasExpensiveRandomAccess());

    const Counters counters;
    for (int i = 0; i < 6; ++i)
    {
        FLOAR_CHECK(reader->ExtractToMemory(EntryName(i)) == Bytes(EntryData(i)));
    }
    // As before: one archive open and a scan from the first header per call.
    FLOAR_CHECK(counters.Handles() == 6);
    FLOAR_CHECK(counters.Headers() == 1 + 2 + 3 + 4 + 5 + 6);
    const auto collected = Forward(*reader, { EntryName(4), EntryName(1), EntryName(2) }, {}, ExtractPriority::Normal, 2);
    FLOAR_CHECK(collected.result == IArchiveReader::ExtractResult::EndedBySink);
    FLOAR_CHECK(collected.order == (std::vector<std::wstring> { EntryName(1), EntryName(2) }));
    const auto stats = pool->Stats();
    FLOAR_CHECK(stats.opens == 0);
    FLOAR_CHECK(stats.idle == 0);
}
#endif
