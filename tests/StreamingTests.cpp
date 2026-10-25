#include "TestHarness.h"

#include "ArchiveReader.h"
#include "ArchiveReaderCache.h"
#include "TestArchives.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// Cancellable directory enumeration (filesystem and archives, including an
// archive open interrupted by stop) and batch extraction serviced by one
// archive scan: completion, cancellation at entry boundaries, missing
// entries, and concurrent readers.

namespace
{
    using namespace Floar;

    std::filesystem::path TempDir(const std::wstring& tag)
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        const std::filesystem::path dir = std::filesystem::path(tempPath) /
            (L"FloarStream_" + std::to_wstring(GetCurrentProcessId()) + L"_" + tag + L"_" + std::to_wstring(GetTickCount64()));
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

    std::wstring Name(int i)
    {
        wchar_t buffer[16] {};
        swprintf_s(buffer, L"f%02d.txt", i);
        return buffer;
    }

    std::stop_source Stopped()
    {
        std::stop_source stop;
        stop.request_stop();
        return stop;
    }
}

FLOAR_TEST(StreamEnumerate_FilesystemStopsBetweenEntries)
{
    ScopedDir dir { TempDir(L"fs") };
    for (int i = 0; i < 30; ++i)
    {
        std::ofstream(dir.path / Name(i)) << i;
    }

    int count = 0;
    FLOAR_CHECK(VirtualFileSystem::EnumerateDirectory(VirtualPath(dir.path), {}, [&](VirtualFileEntry&&) { ++count; }));
    FLOAR_CHECK(count == 30);

    std::stop_source stop;
    count = 0;
    FLOAR_CHECK(!VirtualFileSystem::EnumerateDirectory(VirtualPath(dir.path), stop.get_token(), [&](VirtualFileEntry&&)
    {
        if (++count == 5)
        {
            stop.request_stop();
        }
    }));
    FLOAR_CHECK(count == 5);

    count = 0;
    FLOAR_CHECK(!VirtualFileSystem::EnumerateDirectory(VirtualPath(dir.path), Stopped().get_token(), [&](VirtualFileEntry&&) { ++count; }));
    FLOAR_CHECK(count == 0);
    // A missing folder completes with no entries.
    FLOAR_CHECK(VirtualFileSystem::EnumerateDirectory(VirtualPath(dir.path / L"missing"), {}, [&](VirtualFileEntry&&) { ++count; }));
    FLOAR_CHECK(count == 0);
}

#if FLOAR_ENABLE_COMMON_ARCHIVES
namespace
{
    std::vector<std::pair<std::wstring, std::string>> FortyFiles()
    {
        std::vector<std::pair<std::wstring, std::string>> files;
        for (int i = 0; i < 40; ++i)
        {
            files.emplace_back(Name(i), "data " + std::to_string(i));
        }
        files.emplace_back(L"Sub/a.txt", "sub");
        return files;
    }
}

FLOAR_TEST(StreamEnumerate_ArchiveCompletesAndStopsBetweenEntries)
{
    ScopedDir dir { TempDir(L"zip") };
    const auto zip = dir.path / L"forty.zip";
    FLOAR_REQUIRE(FloarTest::WriteZip(zip, FortyFiles(), {}));

    // Stopped before anything: the archive is not even opened.
    const size_t opensBefore = ArchiveReaderFactory::OpenCountForTesting();
    int count = 0;
    FLOAR_CHECK(!VirtualFileSystem::EnumerateDirectory(VirtualPath(zip), Stopped().get_token(), [&](VirtualFileEntry&&) { ++count; }));
    FLOAR_CHECK(count == 0);
    FLOAR_CHECK(ArchiveReaderFactory::OpenCountForTesting() == opensBefore);

    FLOAR_CHECK(VirtualFileSystem::EnumerateDirectory(VirtualPath(zip), {}, [&](VirtualFileEntry&&) { ++count; }));
    FLOAR_CHECK(count == 41);   // 40 files and "Sub"

    std::stop_source stop;
    count = 0;
    FLOAR_CHECK(!VirtualFileSystem::EnumerateDirectory(VirtualPath(zip), stop.get_token(), [&](VirtualFileEntry&&)
    {
        if (++count == 10)
        {
            stop.request_stop();
        }
    }));
    FLOAR_CHECK(count == 10);

    count = 0;
    FLOAR_CHECK(VirtualFileSystem::EnumerateDirectory(VirtualPath(zip, L"sub"), {}, [&](VirtualFileEntry&& entry)
    {
        ++count;
        FLOAR_CHECK(entry.path == VirtualPath(zip, L"Sub/a.txt"));
    }));
    FLOAR_CHECK(count == 1);
}

FLOAR_TEST(StreamEnumerate_InterruptedOpenReturnsAndCachesNothing)
{
    ScopedDir dir { TempDir(L"open") };
    const auto zip = dir.path / L"interrupted.zip";
    FLOAR_REQUIRE(FloarTest::WriteZip(zip, FortyFiles(), {}));

    FLOAR_CHECK(ArchiveReaderFactory::Open(zip, Stopped().get_token()) == nullptr);

    ArchiveReaderCache cache(4);
    FLOAR_CHECK(cache.Get(zip, Stopped().get_token()) == nullptr);
    FLOAR_CHECK(cache.Size() == 0);
    const auto reader = cache.Get(zip);
    FLOAR_REQUIRE(reader != nullptr);
    FLOAR_CHECK(reader->Entries().size() == 41);
    // Once cached, a stopped lookup still gets the reader (no open needed).
    FLOAR_CHECK(cache.Get(zip, Stopped().get_token()) == reader);
}

namespace
{
    // Requests stop when the libarchive reader announces its scan (after the
    // factory's own stop check), and records how many entries the scan found.
    std::stop_source* g_stopWhenScanStarts = nullptr;
    std::string g_scanResult;

    void StopWhenScanStarts(Log::Level, const std::string& message)
    {
        if (message.find("[LibArchiveReader] Opening") != std::string::npos && g_stopWhenScanStarts)
        {
            g_stopWhenScanStarts->request_stop();
        }
        if (message.find("[LibArchiveReader] Scanned") != std::string::npos)
        {
            g_scanResult = message;
        }
    }
}

FLOAR_TEST(StreamEnumerate_ScanStoppedMidwayIsDiscarded)
{
    ScopedDir dir { TempDir(L"scan") };
    const auto zip = dir.path / L"scan.zip";
    FLOAR_REQUIRE(FloarTest::WriteZip(zip, FortyFiles(), {}));

    std::stop_source stop;
    g_stopWhenScanStarts = &stop;
    g_scanResult.clear();
    Log::SetSink(&StopWhenScanStarts);
    const auto reader = ArchiveReaderFactory::Open(zip, stop.get_token());
    Log::SetSink(nullptr);
    g_stopWhenScanStarts = nullptr;

    FLOAR_CHECK(stop.stop_requested());
    FLOAR_CHECK(reader == nullptr);
    // The scan itself stopped before reading any header.
    FLOAR_CHECK(g_scanResult.find("found 0 entries") != std::string::npos);
}

namespace
{
    struct BatchFixture
    {
        ScopedDir dir { TempDir(L"batch") };
        std::filesystem::path zip = dir.path / L"batch.zip";
        bool written = FloarTest::WriteZip(zip,
            { { L"a.txt", "alpha" }, { L"b.txt", "bravo" }, { L"c.txt", "charlie" },
              { L"d.txt", "delta" }, { L"Folder/f.txt", "foxtrot" } },
            { L"Folder/" });
    };
}

FLOAR_TEST(StreamBatch_OneScanServicesTheWholeBatch)
{
    BatchFixture fixture;
    FLOAR_REQUIRE(fixture.written);
    const auto reader = ArchiveReaderFactory::Open(fixture.zip);
    FLOAR_REQUIRE(reader != nullptr);

    const std::vector<std::wstring> request = {
        L"Folder\\F.TXT", L"c.txt", L"missing.txt", L"A.TXT", L"a.txt", L"Folder",
    };
    std::map<std::wstring, std::vector<std::string>> delivered;
    const size_t handlesBefore = ArchiveReaderFactory::DataHandleOpenCountForTesting();
    FLOAR_CHECK(reader->ExtractBatch(request, {}, [&](const std::wstring& path, std::vector<uint8_t>&& data)
    {
        delivered[path].emplace_back(data.begin(), data.end());
    }));
    FLOAR_CHECK(ArchiveReaderFactory::DataHandleOpenCountForTesting() == handlesBefore + 1);   // one scan

    FLOAR_CHECK(delivered.size() == request.size());
    for (const auto& path : request)
    {
        FLOAR_CHECK(delivered[path].size() == 1);   // each request exactly once
    }
    FLOAR_CHECK(delivered[L"Folder\\F.TXT"][0] == "foxtrot");
    FLOAR_CHECK(delivered[L"c.txt"][0] == "charlie");
    FLOAR_CHECK(delivered[L"A.TXT"][0] == "alpha");
    FLOAR_CHECK(delivered[L"a.txt"][0] == "alpha");
    FLOAR_CHECK(delivered[L"missing.txt"][0].empty());
    FLOAR_CHECK(delivered[L"Folder"][0].empty());   // a directory

    // Entry-by-entry extraction pays one scan per entry.
    const size_t handlesSingle = ArchiveReaderFactory::DataHandleOpenCountForTesting();
    FLOAR_CHECK(reader->ExtractToMemory(L"b.txt").size() == 5);
    FLOAR_CHECK(reader->ExtractToMemory(L"d.txt").size() == 5);
    FLOAR_CHECK(ArchiveReaderFactory::DataHandleOpenCountForTesting() == handlesSingle + 2);
}

FLOAR_TEST(StreamBatch_StopsAtTheNextEntryBoundary)
{
    BatchFixture fixture;
    FLOAR_REQUIRE(fixture.written);
    const auto reader = ArchiveReaderFactory::Open(fixture.zip);
    FLOAR_REQUIRE(reader != nullptr);
    const std::vector<std::wstring> request = { L"a.txt", L"b.txt", L"c.txt", L"d.txt" };

    std::stop_source stop;
    int calls = 0;
    FLOAR_CHECK(!reader->ExtractBatch(request, stop.get_token(), [&](const std::wstring&, std::vector<uint8_t>&&)
    {
        ++calls;
        stop.request_stop();
    }));
    FLOAR_CHECK(calls == 1);

    // Stopped up front: nothing is read or delivered.
    const size_t handlesBefore = ArchiveReaderFactory::DataHandleOpenCountForTesting();
    calls = 0;
    FLOAR_CHECK(!reader->ExtractBatch(request, Stopped().get_token(), [&](const std::wstring&, std::vector<uint8_t>&&) { ++calls; }));
    FLOAR_CHECK(calls == 0);
    FLOAR_CHECK(ArchiveReaderFactory::DataHandleOpenCountForTesting() == handlesBefore);
}

FLOAR_TEST(StreamBatch_StopFromACallbackEndsAliasesAndMissingEntries)
{
    BatchFixture fixture;
    FLOAR_REQUIRE(fixture.written);
    const auto reader = ArchiveReaderFactory::Open(fixture.zip);
    FLOAR_REQUIRE(reader != nullptr);

    // Three spellings of one entry: a stop from the first delivery ends the batch.
    std::stop_source aliasStop;
    int calls = 0;
    FLOAR_CHECK(!reader->ExtractBatch({ L"a.txt", L"A.TXT", L"/a.txt" }, aliasStop.get_token(), [&](const std::wstring&, std::vector<uint8_t>&&)
    {
        ++calls;
        aliasStop.request_stop();
    }));
    FLOAR_CHECK(calls == 1);

    // Only missing entries (no scan): the same, and the call reports the stop.
    std::stop_source missingStop;
    calls = 0;
    FLOAR_CHECK(!reader->ExtractBatch({ L"x.txt", L"y.txt", L"z.txt" }, missingStop.get_token(), [&](const std::wstring&, std::vector<uint8_t>&&)
    {
        ++calls;
        missingStop.request_stop();
    }));
    FLOAR_CHECK(calls == 1);

    // A stop from the last delivery is reported too.
    std::stop_source lastStop;
    calls = 0;
    FLOAR_CHECK(!reader->ExtractBatch({ L"b.txt" }, lastStop.get_token(), [&](const std::wstring&, std::vector<uint8_t>&&)
    {
        ++calls;
        lastStop.request_stop();
    }));
    FLOAR_CHECK(calls == 1);
}

FLOAR_TEST(StreamBatch_ThrowingCallbackReleasesTheArchive)
{
    BatchFixture fixture;
    FLOAR_REQUIRE(fixture.written);
    auto reader = ArchiveReaderFactory::Open(fixture.zip);
    FLOAR_REQUIRE(reader != nullptr);

    bool caught = false;
    try
    {
        (void)reader->ExtractBatch({ L"a.txt", L"b.txt" }, {}, [](const std::wstring&, std::vector<uint8_t>&&)
        {
            throw std::runtime_error("callback failure");
        });
    }
    catch (const std::runtime_error&)
    {
        caught = true;
    }
    FLOAR_CHECK(caught);

    // The libarchive handle was freed: the archive is not held open, so it can
    // be deleted (Windows refuses while a handle without delete sharing exists).
    std::error_code ec;
    FLOAR_CHECK(std::filesystem::remove(fixture.zip, ec) && !ec);
    // The reader keeps no batch state.
    FLOAR_CHECK(reader->Entries().size() == 6);
}

FLOAR_TEST(StreamBatch_ConcurrentReadersShareOneReader)
{
    BatchFixture fixture;
    FLOAR_REQUIRE(fixture.written);
    const auto reader = ArchiveReaderCache::Shared().Get(fixture.zip);
    FLOAR_REQUIRE(reader != nullptr);
    const std::vector<std::wstring> request = { L"d.txt", L"a.txt", L"Folder/f.txt", L"b.txt", L"c.txt" };
    const std::map<std::wstring, std::string> expected = {
        { L"a.txt", "alpha" }, { L"b.txt", "bravo" }, { L"c.txt", "charlie" },
        { L"d.txt", "delta" }, { L"Folder/f.txt", "foxtrot" },
    };

    std::vector<std::map<std::wstring, std::string>> results(6);
    std::vector<std::thread> threads;
    for (auto& result : results)
    {
        threads.emplace_back([&]
        {
            for (int round = 0; round < 5; ++round)
            {
                (void)reader->ExtractBatch(request, {}, [&](const std::wstring& path, std::vector<uint8_t>&& data)
                {
                    result[path] = std::string(data.begin(), data.end());
                });
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    for (const auto& result : results)
    {
        FLOAR_CHECK(result == expected);
    }
}
#endif
