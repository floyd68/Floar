// Opt-in forward-cursor benchmark for solid archives (not part of ctest or the
// shipped library). Built with the Floar tests as FloarSolidCursorBench.
//
//   FloarSolidCursorBench <archive> <inner folder> [restart count]
//
// Reads the files of one folder of `archive` (a 7z or RAR; generate a
// representative one with 7-Zip, for example LZMA2 with a 64 MiB dictionary)
// one ExtractToMemory call at a time, in archive order, as successive
// thumbnail requests do, and prints the latency of each read:
//
// - restart: a pool that retains no cursor (maxIdle 0), so every read opens the
//   archive and decompresses from its start, as Floar did before forward
//   cursors; only the first `restart count` files (default 5), since each one
//   costs the whole stream before it;
// - cursor: the default pool, so every read after the first continues the
//   cursor the previous one left.
//
// It also prints the archive's open and scan time, the archive opens and
// entry headers each mode used, the reader's per-cursor memory estimate, and
// the process's private bytes before the first cursor, while one cursor is
// idle, and after TrimIdle closed it (a measurement of one cursor's
// decompressor memory: run it with nothing else in the process).
//
// The archive is only read.

#include "ArchiveCursorPool.h"
#include "ArchiveReader.h"
#include "VirtualPath.h"

#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Floar::ArchiveCursorLimits;
    using Floar::ArchiveCursorPool;
    using Floar::ArchiveReaderFactory;
    using Floar::IArchiveReader;

    double Ms(Clock::duration duration)
    {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    double PrivateMiB()
    {
        PROCESS_MEMORY_COUNTERS_EX counters {};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        {
            return 0;
        }
        return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
    }

    // Files directly in `folder` (ArchiveInnerPathKey spelling), in archive order.
    std::vector<std::wstring> FolderFiles(const IArchiveReader& reader, const std::wstring& folder)
    {
        const std::wstring prefix = Floar::ArchiveInnerPathKey(folder) + L"/";
        std::vector<std::pair<size_t, std::wstring>> files;
        for (const auto& entry : reader.Entries())
        {
            const std::wstring key = Floar::ArchiveInnerPathKey(entry.name);
            if (entry.isDirectory || key.rfind(prefix, 0) != 0 || key.find(L'/', prefix.size()) != std::wstring::npos)
            {
                continue;
            }
            files.emplace_back(reader.ArchiveOrder(entry.name), entry.name);
        }
        std::sort(files.begin(), files.end());
        std::vector<std::wstring> names;
        for (auto& file : files)
        {
            names.push_back(std::move(file.second));
        }
        return names;
    }

    struct Run
    {
        std::vector<double> latencies;
        size_t opens { 0 };
        size_t headers { 0 };
        size_t bytes { 0 };
    };

    Run ReadInOrder(IArchiveReader& reader, const std::vector<std::wstring>& files, size_t count)
    {
        Run run;
        const size_t opensBefore = ArchiveReaderFactory::DataHandleOpenCountForTesting();
        const size_t headersBefore = ArchiveReaderFactory::ExtractionHeaderCountForTesting();
        for (size_t i = 0; i < (std::min)(count, files.size()); ++i)
        {
            const auto start = Clock::now();
            const auto data = reader.ExtractToMemory(files[i]);
            run.latencies.push_back(Ms(Clock::now() - start));
            run.bytes += data.size();
            if (data.empty())
            {
                std::fprintf(stderr, "empty read: entry %zu\n", i);
            }
        }
        run.opens = ArchiveReaderFactory::DataHandleOpenCountForTesting() - opensBefore;
        run.headers = ArchiveReaderFactory::ExtractionHeaderCountForTesting() - headersBefore;
        return run;
    }

    void Print(const char* name, const Run& run)
    {
        if (run.latencies.empty())
        {
            return;
        }
        double total = 0;
        for (double latency : run.latencies)
        {
            total += latency;
        }
        std::vector<double> rest(run.latencies.begin() + 1, run.latencies.end());
        std::sort(rest.begin(), rest.end());
        std::printf("%-8s %zu reads, %.1f MiB: first %.1f ms", name, run.latencies.size(),
            static_cast<double>(run.bytes) / (1024.0 * 1024.0), run.latencies.front());
        if (!rest.empty())
        {
            std::printf(", later median %.2f ms, max %.2f ms", rest[rest.size() / 2], rest.back());
        }
        std::printf(", total %.1f ms; %zu archive opens, %zu headers read\n", total, run.opens, run.headers);
        std::printf("         per read (ms):");
        for (double latency : run.latencies)
        {
            std::printf(" %.1f", latency);
        }
        std::printf("\n");
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: FloarSolidCursorBench <archive> <inner folder> [restart count]\n");
        return 2;
    }
    const std::filesystem::path archive = argv[1];
    const std::wstring folder = argv[2];
    const size_t restartCount = (argc > 3) ? static_cast<size_t>(std::wcstoul(argv[3], nullptr, 10)) : 5;

    // Restart: no cursor is ever retained.
    ArchiveCursorLimits noRetention;
    noRetention.maxIdle = 0;
    const auto restartPool = std::make_shared<ArchiveCursorPool>(noRetention);
    auto start = Clock::now();
    auto restartReader = ArchiveReaderFactory::Open(archive, {}, restartPool);
    const double openMs = Ms(Clock::now() - start);
    if (!restartReader)
    {
        std::fprintf(stderr, "cannot open the archive\n");
        return 1;
    }
    const auto files = FolderFiles(*restartReader, folder);
    if (files.empty())
    {
        std::fprintf(stderr, "no files in that folder\n");
        return 1;
    }
    std::printf("archive %.1f MiB, %zu entries, %s; open and scan %.1f ms\n",
        static_cast<double>(std::filesystem::file_size(archive)) / (1024.0 * 1024.0), restartReader->Entries().size(),
        restartReader->HasExpensiveRandomAccess() ? "expensive random access" : "cheap random access", openMs);
    std::printf("folder: %zu files, archive order %zu..%zu\n", files.size(),
        restartReader->ArchiveOrder(files.front()), restartReader->ArchiveOrder(files.back()));

    Print("restart", ReadInOrder(*restartReader, files, restartCount));
    restartReader.reset();

    const auto pool = std::make_shared<ArchiveCursorPool>();
    auto reader = ArchiveReaderFactory::Open(archive, {}, pool);
    const double before = PrivateMiB();
    // One read, then measure while its cursor is idle (the data is freed).
    Run cursor = ReadInOrder(*reader, files, 1);
    const double idle = PrivateMiB();
    const auto stats = pool->Stats();
    const Run rest = [&]
    {
        std::vector<std::wstring> later(files.begin() + 1, files.end());
        return ReadInOrder(*reader, later, later.size());
    }();
    cursor.latencies.insert(cursor.latencies.end(), rest.latencies.begin(), rest.latencies.end());
    cursor.opens += rest.opens;
    cursor.headers += rest.headers;
    cursor.bytes += rest.bytes;
    Print("cursor", cursor);
    (void)pool->TrimIdle();
    const double after = PrivateMiB();
    std::printf("cursor memory: estimate %.1f MiB; private bytes %.1f MiB before the first cursor, %.1f MiB with one idle cursor (+%.1f), %.1f MiB after TrimIdle\n",
        static_cast<double>(stats.memoryBytes) / (1024.0 * 1024.0), before, idle, idle - before, after);
    const auto final = pool->Stats();
    std::printf("pool: opens %llu, resumes %llu, restarts %llu, headers %llu, entries %llu\n",
        static_cast<unsigned long long>(final.opens), static_cast<unsigned long long>(final.resumes),
        static_cast<unsigned long long>(final.restarts), static_cast<unsigned long long>(final.headersRead),
        static_cast<unsigned long long>(final.entriesRead));
    return 0;
}
