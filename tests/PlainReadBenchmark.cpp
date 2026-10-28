// Opt-in plain-file read throughput benchmark (not part of ctest or the
// shipped library). Built with the Floar tests as FloarPlainReadBench.
//
//   FloarPlainReadBench [files] [minMiB] [maxMiB] [reps]
//
// Generates `files` incompressible files of minMiB..maxMiB (fixed seed, so
// every run uses the same sizes) in a fresh folder under %TEMP%, reads them all
// through VirtualFileSystem::ReadFile and prints MiB/s per repetition and the
// median, for both overloads, in two cache states:
//
// - warm: every file was read once before the timed passes, so the data is
//   served from the system file cache;
// - cold: before every timed pass the files are rewritten with unbuffered,
//   write-through I/O (FILE_FLAG_NO_BUFFERING), which purges their pages from
//   the system file cache, so the pass reads from the device. No elevation is
//   needed (the standby list is not purged); the first read of each file then
//   misses the cache.
//
// The folder is deleted on exit.

#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <stop_token>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct BenchFile
    {
        std::filesystem::path path;
        uint64_t size { 0 };
    };

    struct AlignedBuffer
    {
        void* data { nullptr };
        size_t size { 0 };
        explicit AlignedBuffer(size_t bytes)
            : data(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
            , size(bytes)
        {
        }
        ~AlignedBuffer()
        {
            if (data)
            {
                VirtualFree(data, 0, MEM_RELEASE);
            }
        }
    };

    // Writes `size` bytes of `content` without the system cache (any cached
    // pages of the file are purged), then trims the file to `size`.
    bool WriteUncached(const std::filesystem::path& path, const AlignedBuffer& content, uint64_t size)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        constexpr uint64_t kAlign = 64 * 1024;   // a multiple of any sector size
        const uint64_t padded = (size + kAlign - 1) / kAlign * kAlign;
        bool ok = true;
        uint64_t written = 0;
        while (ok && written < padded)
        {
            const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(padded - written, 8ull * 1024 * 1024));
            DWORD done = 0;
            ok = ::WriteFile(file, static_cast<const uint8_t*>(content.data) + written, chunk, &done, nullptr) && done == chunk;
            written += done;
        }
        if (ok)
        {
            FILE_END_OF_FILE_INFO eof {};
            eof.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
            ok = SetFileInformationByHandle(file, FileEndOfFileInfo, &eof, sizeof(eof)) != FALSE;
        }
        CloseHandle(file);
        return ok;
    }

    double Median(std::vector<double> values)
    {
        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        return (n % 2) ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
    }

    // One pass over every file; returns MiB/s, or a negative value when a read
    // returned the wrong size.
    double ReadPass(const std::vector<BenchFile>& files, bool stopAware)
    {
        uint64_t total = 0;
        const auto start = Clock::now();
        for (const auto& file : files)
        {
            const Floar::VirtualPath path(file.path);
            const auto data = stopAware
                ? Floar::VirtualFileSystem::ReadFile(path, std::stop_token {})
                : Floar::VirtualFileSystem::ReadFile(path);
            if (data.size() != file.size)
            {
                return -1.0;
            }
            total += data.size();
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        return (static_cast<double>(total) / (1024.0 * 1024.0)) / seconds;
    }
}

int main(int argc, char** argv)
{
    const int fileCount = (argc > 1) ? std::atoi(argv[1]) : 30;
    const int minMiB = (argc > 2) ? std::atoi(argv[2]) : 2;
    const int maxMiB = (argc > 3) ? std::atoi(argv[3]) : 20;
    const int reps = (argc > 4) ? std::atoi(argv[4]) : 9;
    if (fileCount <= 0 || minMiB <= 0 || maxMiB < minMiB || reps <= 0)
    {
        std::fprintf(stderr, "usage: FloarPlainReadBench [files] [minMiB] [maxMiB] [reps]\n");
        return 2;
    }

    // A directory this run creates itself (CreateDirectoryW fails on an
    // existing one, e.g. left by an interrupted run whose PID was reused), so
    // the cleanup below never touches files this run did not write.
    wchar_t tempPath[MAX_PATH] {};
    GetTempPathW(MAX_PATH, tempPath);
    std::filesystem::path dir;
    for (int attempt = 0; attempt < 100 && dir.empty(); ++attempt)
    {
        const std::filesystem::path candidate = std::filesystem::path(tempPath) /
            (L"FloarPlainReadBench_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(attempt));
        if (CreateDirectoryW(candidate.c_str(), nullptr))
        {
            dir = candidate;
        }
        else if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            break;
        }
    }
    if (dir.empty())
    {
        std::fprintf(stderr, "could not create a fresh benchmark directory\n");
        return 1;
    }

    std::mt19937_64 rng(20261028);
    std::uniform_int_distribution<uint64_t> sizeDist(uint64_t(minMiB) << 20, uint64_t(maxMiB) << 20);
    std::vector<BenchFile> files;
    uint64_t totalBytes = 0;
    uint64_t largest = 0;
    for (int i = 0; i < fileCount; ++i)
    {
        BenchFile file { dir / (L"img" + std::to_wstring(i) + L".jpg"), sizeDist(rng) };
        totalBytes += file.size;
        largest = std::max(largest, file.size);
        files.push_back(std::move(file));
    }
    // Incompressible content, as in a JPEG; one buffer serves every file.
    AlignedBuffer content(static_cast<size_t>(largest + 64 * 1024));
    {
        auto* words = static_cast<uint64_t*>(content.data);
        for (size_t i = 0; i < content.size / sizeof(uint64_t); ++i)
        {
            words[i] = rng();
        }
    }

    auto rewriteAll = [&]
    {
        for (const auto& file : files)
        {
            if (!WriteUncached(file.path, content, file.size))
            {
                return false;
            }
        }
        return true;
    };

    int status = 0;
    if (!rewriteAll())
    {
        std::fprintf(stderr, "could not write the benchmark files\n");
        status = 1;
    }
    else
    {
        std::printf("%d files, %.1f MiB total, %d-%d MiB each, %d reps\n",
            fileCount, static_cast<double>(totalBytes) / (1024.0 * 1024.0), minMiB, maxMiB, reps);

        struct Series
        {
            const char* name;
            bool stopAware;
            bool cold;
            std::vector<double> results;
        };
        std::vector<Series> series {
            { "warm ReadFile(path)      ", false, false, {} },
            { "warm ReadFile(path, stop)", true, false, {} },
            { "cold ReadFile(path)      ", false, true, {} },
            { "cold ReadFile(path, stop)", true, true, {} },
        };
        // Warm-up read so the first warm pass is served from the cache.
        (void)ReadPass(files, false);
        // Interleave the series so drift affects them alike.
        for (int rep = 0; rep < reps && status == 0; ++rep)
        {
            for (auto& s : series)
            {
                if (s.cold && !rewriteAll())
                {
                    std::fprintf(stderr, "could not rewrite the benchmark files\n");
                    status = 1;
                    break;
                }
                if (!s.cold)
                {
                    (void)ReadPass(files, s.stopAware);   // re-warm after a cold pass
                }
                const double mibps = ReadPass(files, s.stopAware);
                if (mibps < 0)
                {
                    std::fprintf(stderr, "%s: a read returned the wrong size\n", s.name);
                    status = 1;
                    break;
                }
                s.results.push_back(mibps);
            }
        }
        if (status == 0)
        {
            for (const auto& s : series)
            {
                std::printf("%s median %8.1f MiB/s  [", s.name, Median(s.results));
                for (size_t i = 0; i < s.results.size(); ++i)
                {
                    std::printf("%s%.0f", i ? " " : "", s.results[i]);
                }
                std::printf("]\n");
            }
        }
    }

    // Remove only this run's files and its own directory.
    for (const auto& file : files)
    {
        (void)DeleteFileW(file.path.c_str());
    }
    (void)RemoveDirectoryW(dir.c_str());
    return status;
}
