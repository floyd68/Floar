#include "TestHarness.h"

#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstdio>
#include <future>
#include <stop_token>
#include <string>
#include <vector>

// Plain-file VirtualFileSystem::ReadFile through the plain-file read hook: a
// file that shrinks after the size query or a read that fails partway returns
// empty data in both overloads (never a zero-padded buffer), a pre-canceled
// read opens nothing, a stop between chunks returns empty data after at most
// one more chunk, and normal reads of empty, small, one-chunk and multi-chunk
// files return unchanged bytes in kPlainFileReadChunk chunks.

namespace
{
    using namespace Floar;
    using namespace std::chrono_literals;

    constexpr size_t kChunk = VirtualFileSystem::kPlainFileReadChunk;

    std::filesystem::path TempDir(const std::wstring& tag)
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        // A directory created by this call (CreateDirectoryW fails on an
        // existing one), so ScopedDir's cleanup removes only test files.
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const std::filesystem::path dir = std::filesystem::path(tempPath) /
                (L"FloarPlain_" + std::to_wstring(GetCurrentProcessId()) + L"_" + tag + L"_" + std::to_wstring(attempt));
            if (CreateDirectoryW(dir.c_str(), nullptr))
            {
                return dir;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS)
            {
                break;
            }
        }
        return {};
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

    // Installs a plain-file read hook for the scope of one test.
    struct ScopedHook
    {
        explicit ScopedHook(VirtualFileSystem::PlainFileReadHook hook)
        {
            VirtualFileSystem::SetPlainFileReadHook(std::move(hook));
        }
        ~ScopedHook()
        {
            VirtualFileSystem::SetPlainFileReadHook({});
        }
    };

    // Deterministic bytes with no zero run, so a zero-padded tail is visible.
    std::vector<uint8_t> Pattern(size_t size)
    {
        std::vector<uint8_t> data(size);
        uint32_t state = 0x9E3779B9u ^ static_cast<uint32_t>(size);
        for (auto& byte : data)
        {
            state = state * 1664525u + 1013904223u;
            byte = static_cast<uint8_t>((state >> 24) | 1u);
        }
        return data;
    }

    std::filesystem::path WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& data)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return path;
    }

    // Truncates the file through a second handle while the read keeps its own.
    bool Truncate(const std::filesystem::path& path, uint64_t size)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        FILE_END_OF_FILE_INFO eof {};
        eof.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
        const bool ok = SetFileInformationByHandle(file, FileEndOfFileInfo, &eof, sizeof(eof)) != FALSE;
        CloseHandle(file);
        return ok;
    }

    using ReadFn = std::function<std::vector<uint8_t>(const VirtualPath&)>;

    // Both public overloads; ReadFile(path, stop, cache) shares the plain path.
    std::vector<std::pair<const char*, ReadFn>> BothOverloads()
    {
        return {
            { "ReadFile(path)", [](const VirtualPath& path) { return VirtualFileSystem::ReadFile(path); } },
            { "ReadFile(path, stop)", [](const VirtualPath& path) { return VirtualFileSystem::ReadFile(path, std::stop_token {}); } },
        };
    }

    std::stop_source Stopped()
    {
        std::stop_source stop;
        stop.request_stop();
        return stop;
    }
}

FLOAR_TEST(Plain_ShrinkingFileReturnsEmptyInBothOverloads)
{
    ScopedDir dir { TempDir(L"shrink") };
    FLOAR_REQUIRE(!dir.path.empty());
    const size_t size = 2 * kChunk + kChunk / 2;
    const auto original = Pattern(size);

    // The file shrinks after the size query: before the first chunk (the read
    // ends inside it), before a later chunk, and to a chunk boundary (the next
    // read returns no bytes at all).
    for (const uint64_t shrinkAt : { uint64_t(0), uint64_t(kChunk), uint64_t(2 * kChunk) })
    {
        for (const uint64_t newSize : { shrinkAt + 100, shrinkAt })
        {
            for (const auto& [name, read] : BothOverloads())
            {
                const auto path = WriteBytes(dir.path / L"shrink.bin", original);
                std::atomic<int> truncations { 0 };
                std::vector<uint8_t> data;
                {
                    ScopedHook hook({ {}, [&](uint64_t offset, size_t) -> uint32_t
                    {
                        if (offset == shrinkAt && Truncate(path, newSize))
                        {
                            ++truncations;
                        }
                        return ERROR_SUCCESS;
                    } });
                    data = read(VirtualPath(path));
                }
                if (truncations != 1 || !data.empty())
                {
                    std::fprintf(stderr, "  %s: shrink to %llu before offset %llu returned %zu bytes\n",
                        name, static_cast<unsigned long long>(newSize), static_cast<unsigned long long>(shrinkAt), data.size());
                }
                FLOAR_CHECK(truncations == 1);
                FLOAR_CHECK(data.empty());
            }
        }
    }
}

FLOAR_TEST(Plain_MidReadFailureReturnsEmptyInBothOverloads)
{
    ScopedDir dir { TempDir(L"fail") };
    FLOAR_REQUIRE(!dir.path.empty());
    const size_t size = 3 * kChunk + 12345;
    const auto path = WriteBytes(dir.path / L"fail.bin", Pattern(size));

    // An OS error on the first, a middle and the last chunk, including the
    // ERROR_OPERATION_ABORTED a CancelSynchronousIo produces, without a stop.
    for (const uint32_t error : { uint32_t(ERROR_OPERATION_ABORTED), uint32_t(ERROR_IO_DEVICE), uint32_t(ERROR_NETNAME_DELETED) })
    {
        for (const uint64_t failAt : { uint64_t(0), uint64_t(kChunk), uint64_t(3 * kChunk) })
        {
            for (const auto& [name, read] : BothOverloads())
            {
                std::atomic<int> chunks { 0 };
                std::vector<uint8_t> data;
                {
                    ScopedHook hook({ {}, [&](uint64_t offset, size_t) -> uint32_t
                    {
                        ++chunks;
                        return (offset == failAt) ? error : ERROR_SUCCESS;
                    } });
                    data = read(VirtualPath(path));
                }
                if (!data.empty())
                {
                    std::fprintf(stderr, "  %s: error %u at offset %llu returned %zu bytes\n",
                        name, error, static_cast<unsigned long long>(failAt), data.size());
                }
                FLOAR_CHECK(data.empty());
                // No chunk is read after the failed one.
                FLOAR_CHECK(chunks == static_cast<int>(failAt / kChunk) + 1);
            }
        }
    }
    // The file itself is intact: an unhooked read returns all of it.
    FLOAR_CHECK(VirtualFileSystem::ReadFile(VirtualPath(path)) == Pattern(size));
}

FLOAR_TEST(Plain_PreCanceledReadOpensNothing)
{
    ScopedDir dir { TempDir(L"pre") };
    FLOAR_REQUIRE(!dir.path.empty());
    const auto path = WriteBytes(dir.path / L"pre.bin", Pattern(kChunk + 1));
    std::atomic<int> opens { 0 };
    std::atomic<int> chunks { 0 };
    ScopedHook hook({
        [&](const std::filesystem::path&) { ++opens; },
        [&](uint64_t, size_t) -> uint32_t { ++chunks; return ERROR_SUCCESS; } });

    FLOAR_CHECK(VirtualFileSystem::ReadFile(VirtualPath(path), Stopped().get_token()).empty());
    FLOAR_CHECK(opens == 0);
    FLOAR_CHECK(chunks == 0);
    // The hook sees an unstopped read.
    FLOAR_CHECK(VirtualFileSystem::ReadFile(VirtualPath(path), std::stop_token {}) == Pattern(kChunk + 1));
    FLOAR_CHECK(opens == 1);
    FLOAR_CHECK(chunks == 2);
}

FLOAR_TEST(Plain_StopBetweenChunksReturnsEmptyAfterAtMostOneMoreChunk)
{
    ScopedDir dir { TempDir(L"stop") };
    FLOAR_REQUIRE(!dir.path.empty());
    const size_t size = 4 * kChunk + 777;
    const int chunkCount = 5;
    const auto path = WriteBytes(dir.path / L"stop.bin", Pattern(size));

    // A stop requested while chunk k is about to be read (from the hook, the
    // latest point before that read) ends the read once chunk k is done: no
    // further chunk is read and no data is returned, including after the last
    // chunk.
    for (int stopAt = 0; stopAt < chunkCount; ++stopAt)
    {
        std::stop_source stop;
        std::atomic<int> chunks { 0 };
        std::vector<uint8_t> data;
        {
            ScopedHook hook({ {}, [&](uint64_t offset, size_t) -> uint32_t
            {
                ++chunks;
                if (offset == uint64_t(stopAt) * kChunk)
                {
                    stop.request_stop();
                }
                return ERROR_SUCCESS;
            } });
            data = VirtualFileSystem::ReadFile(VirtualPath(path), stop.get_token());
        }
        FLOAR_CHECK(data.empty());
        FLOAR_CHECK(chunks == stopAt + 1);
    }

    // The same with the stop requested by another thread while a chunk read
    // is held: the reader returns empty data without reading the next chunk.
    {
        std::stop_source stop;
        std::atomic<int> chunks { 0 };
        std::promise<void> holding;
        std::promise<void> release;
        auto released = release.get_future().share();
        ScopedHook hook({ {}, [&](uint64_t offset, size_t) -> uint32_t
        {
            ++chunks;
            if (offset == kChunk)
            {
                holding.set_value();
                released.wait_for(10s);
            }
            return ERROR_SUCCESS;
        } });
        auto read = std::async(std::launch::async, [&] { return VirtualFileSystem::ReadFile(VirtualPath(path), stop.get_token()); });
        // Nothing may throw while the future is pending (its destructor waits
        // for the reader): the gate is released whatever happened, and a
        // reader that still does not return ends the process instead of
        // hanging it (it captures this scope, so it cannot be detached).
        const bool held = holding.get_future().wait_for(10s) == std::future_status::ready;
        stop.request_stop();
        release.set_value();
        if (read.wait_for(10s) != std::future_status::ready)
        {
            std::fprintf(stderr, "  FATAL: the stopped plain-file read did not return within 10 s\n");
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 3);
        }
        FLOAR_CHECK(held);
        FLOAR_CHECK(read.get().empty());
        FLOAR_CHECK(chunks == 2);
    }

    // The stop belongs to that read only: an unstopped read returns all bytes.
    FLOAR_CHECK(VirtualFileSystem::ReadFile(VirtualPath(path), std::stop_token {}) == Pattern(size));
}

FLOAR_TEST(Plain_NormalReadsReturnUnchangedBytes)
{
    ScopedDir dir { TempDir(L"bytes") };
    FLOAR_REQUIRE(!dir.path.empty());
    std::stop_source live;
    std::vector<std::pair<const char*, ReadFn>> reads = BothOverloads();
    reads.emplace_back("ReadFile(path, live stop)",
        [&live](const VirtualPath& path) { return VirtualFileSystem::ReadFile(path, live.get_token()); });

    // An empty file returns no data (as before); everything else returns its
    // bytes, read in ceil(size / kPlainFileReadChunk) chunks.
    for (const size_t size : { size_t(0), size_t(1), size_t(4096), kChunk - 1, kChunk, kChunk + 1,
             2 * kChunk, 3 * kChunk + 12345 })
    {
        const auto expected = Pattern(size);
        const auto path = WriteBytes(dir.path / (L"file_" + std::to_wstring(size) + L".bin"), expected);
        for (const auto& [name, read] : reads)
        {
            std::atomic<int> chunks { 0 };
            std::atomic<uint64_t> nextOffset { 0 };
            std::atomic<bool> inOrder { true };
            std::vector<uint8_t> data;
            {
                ScopedHook hook({ {}, [&](uint64_t offset, size_t chunkSize) -> uint32_t
                {
                    ++chunks;
                    inOrder = inOrder && offset == nextOffset && chunkSize <= kChunk;
                    nextOffset = offset + chunkSize;
                    return ERROR_SUCCESS;
                } });
                data = read(VirtualPath(path));
            }
            if (data != expected)
            {
                std::fprintf(stderr, "  %s: %zu-byte file returned %zu bytes\n", name, size, data.size());
            }
            FLOAR_CHECK(data == expected);
            FLOAR_CHECK(chunks == static_cast<int>((size + kChunk - 1) / kChunk));
            FLOAR_CHECK(inOrder);
            FLOAR_CHECK(nextOffset == size);
        }
        // And without a hook installed.
        FLOAR_CHECK(VirtualFileSystem::ReadFile(VirtualPath(path)) == expected);
    }

    // A missing file or a folder returns no data.
    FLOAR_CHECK(VirtualFileSystem::ReadFile(VirtualPath(dir.path / L"missing.bin")).empty());
    FLOAR_CHECK(VirtualFileSystem::ReadFile(VirtualPath(dir.path), std::stop_token {}).empty());
}
