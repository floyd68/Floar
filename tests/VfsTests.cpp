#include "TestHarness.h"

#include "ArchiveReader.h"
#include "ArchiveReaderCache.h"
#include "FloarLog.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "TestArchives.h"

// Path policy (normalization, equality, hashing), the extension check before
// any status query, and the archive-reader cache: reuse across equivalent
// spellings, LRU eviction, change detection, per-archive open coordination, and
// VFS queries that reuse one reader without copying its entry table.

namespace
{
    using namespace Floar;
    using namespace std::chrono_literals;

    std::filesystem::path TempDir(const std::wstring& tag)
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        const std::filesystem::path dir = std::filesystem::path(tempPath) /
            (L"FloarVfs_" + std::to_wstring(GetCurrentProcessId()) + L"_" + tag + L"_" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(dir);
        return dir;
    }

    void WriteText(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    }

    class FakeReader final : public IArchiveReader
    {
    public:
        const std::vector<ArchiveEntry>& Entries() const override { return m_entries; }
        std::vector<uint8_t> ExtractToMemory(const std::wstring&) override { return {}; }
        bool HasEntry(const std::wstring&) override { return false; }
        bool HasDirectory(const std::wstring&) const override { return false; }
        std::wstring GetFormatName() const override { return L"fake"; }

    private:
        std::vector<ArchiveEntry> m_entries {};
    };

    ArchiveReaderCache::Opener FakeOpener()
    {
        return [](const std::filesystem::path&, std::stop_token) { return std::make_shared<FakeReader>(); };
    }
}

FLOAR_TEST(VfsPath_InnerPathsNormalizeAndCompareCaseInsensitively)
{
    const std::filesystem::path archive = L"C:\\data\\pack.zip";
    const VirtualPath a(archive, L"\\Folder\\\\Sub\\Image.DDS\\");
    FLOAR_CHECK(a.archiveInnerPath == L"Folder/Sub/Image.DDS");   // case kept for display
    const VirtualPath b(archive, L"folder/sub/image.dds");
    FLOAR_CHECK(a == b);
    FLOAR_CHECK(std::hash<VirtualPath>()(a) == std::hash<VirtualPath>()(b));
    FLOAR_CHECK(a != VirtualPath(archive, L"folder/sub/other.dds"));
    FLOAR_CHECK(a != VirtualPath(archive));
    FLOAR_CHECK(ArchiveInnerPathKey(L"/A\\B//c/") == L"a/b/c");
    FLOAR_CHECK(NormalizeArchiveInnerPath(L"") == L"");
    FLOAR_CHECK(NormalizeArchiveInnerPath(L"///") == L"");

    // Parse produces the same normalized form.
    const auto parsed = VirtualPath::Parse(L"C:\\data\\pack.zip\\Folder\\Sub\\Image.DDS");
    FLOAR_REQUIRE(parsed.has_value());
    FLOAR_CHECK(parsed->archiveInnerPath == L"Folder/Sub/Image.DDS");
    FLOAR_CHECK(*parsed == a);
}

FLOAR_TEST(VfsPath_EquivalentHostPathsEqualAndHashAlike)
{
    const VirtualPath plain(std::filesystem::path(L"C:\\Data\\Pack.zip"));
    const std::vector<std::filesystem::path> spellings = {
        L"c:/data/pack.zip",
        L"C:\\DATA\\sub\\..\\.\\Pack.ZIP",
        L"C:\\Data\\Pack.zip\\",
    };
    std::unordered_set<VirtualPath> set { plain };
    for (const auto& spelling : spellings)
    {
        const VirtualPath other(spelling);
        FLOAR_CHECK(other == plain);
        FLOAR_CHECK(std::hash<VirtualPath>()(other) == std::hash<VirtualPath>()(plain));
        FLOAR_CHECK(set.count(other) == 1);
    }
    FLOAR_CHECK(VirtualPath(std::filesystem::path(L"C:\\Data\\Pack2.zip")) != plain);
    // Roots keep their separator.
    FLOAR_CHECK(HostPathKey(L"C:\\") == L"c:\\");
    FLOAR_CHECK(HostPathKey(L"C:/dir/") == L"c:\\dir");
    // Host and inner keys combine: same inner path in different archives differs.
    FLOAR_CHECK(VirtualPath(L"C:\\a.zip", L"x.png") != VirtualPath(L"C:\\b.zip", L"x.png"));
}

FLOAR_TEST(VfsFactory_UnsupportedExtensionMakesNoStatusQuery)
{
    const size_t before = ArchiveReaderFactory::StatusQueryCountForTesting();
    // Would be slow to stat (unreachable host), and unsupported anyway.
    FLOAR_CHECK(!ArchiveReaderFactory::IsArchiveFile(L"\\\\floar-test.invalid\\share\\image.png"));
    FLOAR_CHECK(!VirtualPath(std::filesystem::path(L"C:\\no-such-dir\\notes.txt")).IsArchiveFile());
    FLOAR_CHECK(!ArchiveReaderFactory::IsArchiveFile(L"C:\\no-such-dir\\noextension"));
    FLOAR_CHECK(ArchiveReaderFactory::StatusQueryCountForTesting() == before);

    // A supported extension costs exactly one query.
    FLOAR_CHECK(!ArchiveReaderFactory::IsArchiveFile(L"C:\\no-such-dir\\pack.zip"));
    FLOAR_CHECK(ArchiveReaderFactory::StatusQueryCountForTesting() == before + 1);
}

FLOAR_TEST(VfsCache_ReusesReaderForEquivalentSpellings)
{
    const std::filesystem::path dir = TempDir(L"reuse");
    const std::filesystem::path file = dir / L"Pack.zip";
    WriteText(file, "x");
    ArchiveReaderCache cache(4, FakeOpener());

    const auto first = cache.Get(file);
    FLOAR_REQUIRE(first != nullptr);
    std::wstring upper = file.wstring();
    std::transform(upper.begin(), upper.end(), upper.begin(), towupper);
    std::wstring slashed = (dir / L"." / L"Pack.zip").wstring();
    std::replace(slashed.begin(), slashed.end(), L'\\', L'/');
    FLOAR_CHECK(cache.Get(upper) == first);
    FLOAR_CHECK(cache.Get(slashed) == first);
    FLOAR_CHECK(cache.Get(file) == first);
    FLOAR_CHECK(cache.OpenCount() == 1);
    FLOAR_CHECK(cache.Size() == 1);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

FLOAR_TEST(VfsCache_EvictsLeastRecentlyUsedOnly)
{
    const std::filesystem::path dir = TempDir(L"lru");
    const auto a = dir / L"a.zip";
    const auto b = dir / L"b.zip";
    const auto c = dir / L"c.zip";
    WriteText(a, "a");
    WriteText(b, "b");
    WriteText(c, "c");
    ArchiveReaderCache cache(2, FakeOpener());

    const auto readerA = cache.Get(a);
    (void)cache.Get(b);
    FLOAR_CHECK(cache.Get(a) == readerA);   // a is now most recent
    (void)cache.Get(c);                     // evicts b only
    FLOAR_CHECK(cache.Size() == 2);
    FLOAR_CHECK(cache.OpenCount() == 3);
    FLOAR_CHECK(cache.Get(a) == readerA);   // still cached
    FLOAR_CHECK(cache.OpenCount() == 3);
    (void)cache.Get(b);                     // reopened
    FLOAR_CHECK(cache.OpenCount() == 4);
    FLOAR_CHECK(cache.Size() == 2);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

FLOAR_TEST(VfsCache_ReopensWhenSizeOrWriteTimeChanges)
{
    const std::filesystem::path dir = TempDir(L"identity");
    const auto file = dir / L"pack.zip";
    WriteText(file, "aaaa");
    ArchiveReaderCache cache(4, FakeOpener());

    const auto first = cache.Get(file);
    FLOAR_CHECK(cache.Get(file) == first);   // unchanged
    const auto writeTime = std::filesystem::last_write_time(file);

    // Replaced with a different size but the same write time.
    WriteText(file, "aaaaaaaa");
    std::filesystem::last_write_time(file, writeTime);
    const auto second = cache.Get(file);
    FLOAR_CHECK(second != first);

    // Same size, newer write time.
    WriteText(file, "bbbbbbbb");
    std::filesystem::last_write_time(file, writeTime + 10s);
    const auto third = cache.Get(file);
    FLOAR_CHECK(third != second);
    FLOAR_CHECK(cache.OpenCount() == 3);

    // Metadata unreadable (file gone): the cached reader is kept (documented).
    std::filesystem::remove(file);
    FLOAR_CHECK(cache.Get(file) == third);
    FLOAR_CHECK(cache.OpenCount() == 3);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

FLOAR_TEST(VfsCache_FailedOpenIsNotCached)
{
    const std::filesystem::path dir = TempDir(L"failed");
    const auto file = dir / L"broken.zip";
    WriteText(file, "not an archive");
    ArchiveReaderCache cache(4, [](const std::filesystem::path&, std::stop_token) { return std::shared_ptr<IArchiveReader>(); });
    FLOAR_CHECK(cache.Get(file) == nullptr);
    FLOAR_CHECK(cache.Size() == 0);
    FLOAR_CHECK(cache.Get(file) == nullptr);
    FLOAR_CHECK(cache.OpenCount() == 2);   // retried, not remembered

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

FLOAR_TEST(VfsCache_SlowOpenDoesNotBlockOtherArchives)
{
    const std::filesystem::path dir = TempDir(L"slow");
    const auto slow = dir / L"slow.zip";
    const auto cached = dir / L"cached.zip";
    const auto fresh = dir / L"fresh.zip";
    WriteText(slow, "s");
    WriteText(cached, "c");
    WriteText(fresh, "f");

    std::mutex mutex;
    std::condition_variable changed;
    bool slowEntered = false;
    bool releaseSlow = false;
    std::atomic<int> slowOpens { 0 };
    ArchiveReaderCache cache(8, [&](const std::filesystem::path& path, std::stop_token) -> std::shared_ptr<IArchiveReader>
    {
        if (path.filename() == L"slow.zip")
        {
            ++slowOpens;
            std::unique_lock<std::mutex> lock(mutex);
            slowEntered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return releaseSlow; });
        }
        return std::make_shared<FakeReader>();
    });
    const auto cachedReader = cache.Get(cached);

    auto slowCall = std::async(std::launch::async, [&] { return cache.Get(slow); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        FLOAR_REQUIRE(changed.wait_for(lock, 5s, [&] { return slowEntered; }));
    }
    // A second caller of the slow archive waits for the same open...
    auto slowWaiter = std::async(std::launch::async, [&] { return cache.Get(slow); });

    // ...while hits on and opens of other archives proceed.
    auto hit = std::async(std::launch::async, [&] { return cache.Get(cached); });
    auto open = std::async(std::launch::async, [&] { return cache.Get(fresh); });
    FLOAR_CHECK(hit.wait_for(2s) == std::future_status::ready);
    FLOAR_CHECK(open.wait_for(2s) == std::future_status::ready);
    FLOAR_CHECK(slowWaiter.wait_for(100ms) == std::future_status::timeout);

    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseSlow = true;
        changed.notify_all();
    }
    const auto slowReader = slowCall.get();
    FLOAR_CHECK(slowReader != nullptr);
    FLOAR_CHECK(slowWaiter.get() == slowReader);
    FLOAR_CHECK(slowOpens == 1);
    if (hit.valid() && hit.wait_for(5s) == std::future_status::ready)
    {
        FLOAR_CHECK(hit.get() == cachedReader);
    }
    if (open.valid())
    {
        (void)open.get();
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

FLOAR_TEST(VfsCache_EvictionNeverSplitsAnOpenInProgress)
{
    const std::filesystem::path dir = TempDir(L"evict-open");
    const auto a = dir / L"a.zip";
    const auto b = dir / L"b.zip";
    WriteText(a, "a");
    WriteText(b, "b");

    std::mutex mutex;
    std::condition_variable changed;
    bool aEntered = false;
    bool releaseA = false;
    std::atomic<int> aOpens { 0 };
    ArchiveReaderCache cache(1, [&](const std::filesystem::path& path, std::stop_token) -> std::shared_ptr<IArchiveReader>
    {
        if (path.filename() == L"a.zip")
        {
            ++aOpens;
            std::unique_lock<std::mutex> lock(mutex);
            aEntered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return releaseA; });
        }
        return std::make_shared<FakeReader>();
    });

    auto first = std::async(std::launch::async, [&] { return cache.Get(a); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        FLOAR_REQUIRE(changed.wait_for(lock, 5s, [&] { return aEntered; }));
    }
    // Capacity 1: B would evict A, but A is still opening.
    FLOAR_CHECK(cache.Get(b) != nullptr);
    // A second request for A waits for the first open instead of starting another.
    auto second = std::async(std::launch::async, [&] { return cache.Get(a); });
    FLOAR_CHECK(second.wait_for(100ms) == std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseA = true;
        changed.notify_all();
    }
    const auto readerA = first.get();
    FLOAR_CHECK(readerA != nullptr);
    FLOAR_CHECK(second.get() == readerA);
    FLOAR_CHECK(aOpens == 1);
    // Back within capacity once nothing is in use.
    FLOAR_CHECK(cache.Size() == 1);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

namespace
{
    // Records the path it was opened with.
    class PathReader final : public IArchiveReader
    {
    public:
        explicit PathReader(std::filesystem::path path)
            : path(std::move(path))
        {
        }

        const std::vector<ArchiveEntry>& Entries() const override { return m_entries; }
        std::vector<uint8_t> ExtractToMemory(const std::wstring&) override { return {}; }
        bool HasEntry(const std::wstring&) override { return false; }
        bool HasDirectory(const std::wstring&) const override { return false; }
        std::wstring GetFormatName() const override { return L"path"; }

        const std::filesystem::path path;

    private:
        std::vector<ArchiveEntry> m_entries {};
    };

    struct ScopedCurrentPath
    {
        std::filesystem::path saved = std::filesystem::current_path();
        ~ScopedCurrentPath()
        {
            std::error_code ec;
            std::filesystem::current_path(saved, ec);
        }
    };
}

FLOAR_TEST(VfsCache_OpensTheAbsolutePathItIsKeyedBy)
{
    const std::filesystem::path dir = TempDir(L"cwd");
    const auto dirA = dir / L"A";
    const auto dirB = dir / L"B";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);
    WriteText(dirA / L"pack.zip", "a");
    WriteText(dirB / L"pack.zip", "bb");
    ArchiveReaderCache cache(4, [](const std::filesystem::path& path, std::stop_token)
    {
        return std::make_shared<PathReader>(path);
    });

    ScopedCurrentPath restore;
    // Relative lookup first: the reader is opened with the absolute path.
    std::filesystem::current_path(dirA);
    const auto fromRelative = std::dynamic_pointer_cast<PathReader>(cache.Get(L"pack.zip"));
    FLOAR_REQUIRE(fromRelative != nullptr);
    FLOAR_CHECK(fromRelative->path.is_absolute());
    FLOAR_CHECK(std::filesystem::equivalent(fromRelative->path, dirA / L"pack.zip"));

    // After a directory change, the absolute lookup shares that reader, which
    // still refers to A's archive.
    std::filesystem::current_path(dirB);
    FLOAR_CHECK(cache.Get(dirA / L"pack.zip") == fromRelative);
    FLOAR_CHECK(std::filesystem::equivalent(fromRelative->path, dirA / L"pack.zip"));

    // The same relative spelling now names B's archive: a different reader.
    const auto inB = std::dynamic_pointer_cast<PathReader>(cache.Get(L"pack.zip"));
    FLOAR_REQUIRE(inB != nullptr);
    FLOAR_CHECK(inB != fromRelative);
    FLOAR_CHECK(std::filesystem::equivalent(inB->path, dirB / L"pack.zip"));

    std::filesystem::current_path(restore.saved);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

#if FLOAR_ENABLE_COMMON_ARCHIVES
namespace
{
    struct FixtureArchive
    {
        std::filesystem::path dir = TempDir(L"zip");
        std::filesystem::path path = dir / L"Fixture.zip";
        bool written = FloarTest::WriteZip(path,
            { { L"Folder/Sub/a.txt", "a" }, { L"Folder/b.txt", "bb" }, { L"root.txt", "r" } },
            { L"Empty/", L"Folder/" });

        ~FixtureArchive()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };

    std::vector<std::wstring> SortedInnerPaths(const std::vector<VirtualFileEntry>& entries)
    {
        std::vector<std::wstring> names;
        for (const auto& entry : entries)
        {
            names.push_back(ArchiveInnerPathKey(entry.path.archiveInnerPath) + (entry.isDirectory ? L"/" : L""));
        }
        std::sort(names.begin(), names.end());
        return names;
    }
}

FLOAR_TEST(VfsArchive_ExistsAndIsDirectoryReuseOneReader)
{
    FixtureArchive zip;
    FLOAR_REQUIRE(zip.written);
    const size_t opensBefore = ArchiveReaderCache::Shared().OpenCount();
    const size_t factoryOpensBefore = ArchiveReaderFactory::OpenCountForTesting();

    for (int i = 0; i < 10; ++i)
    {
        FLOAR_CHECK(VirtualFileSystem::Exists(VirtualPath(zip.path, L"folder\\SUB\\A.txt")));
        FLOAR_CHECK(VirtualPath(zip.path, L"Folder/b.txt").Exists());
        FLOAR_CHECK(!VirtualPath(zip.path, L"Folder/missing.txt").Exists());
        FLOAR_CHECK(VirtualFileSystem::Exists(VirtualPath(zip.path, L"folder/sub")));   // implicit directory
        FLOAR_CHECK(VirtualFileSystem::IsDirectory(VirtualPath(zip.path, L"FOLDER/Sub/")));
        FLOAR_CHECK(VirtualFileSystem::IsDirectory(VirtualPath(zip.path, L"empty")));   // explicit, no children
        FLOAR_CHECK(!VirtualFileSystem::IsDirectory(VirtualPath(zip.path, L"Folder/b.txt")));
        FLOAR_CHECK(!VirtualFileSystem::IsDirectory(VirtualPath(zip.path, L"Fold")));    // a prefix, not a directory
    }
    // One open in total, through the cache (none bypasses it).
    FLOAR_CHECK(ArchiveReaderCache::Shared().OpenCount() == opensBefore + 1);
    FLOAR_CHECK(ArchiveReaderFactory::OpenCountForTesting() == factoryOpensBefore + 1);

    // The reader exposes its table without copying it.
    const auto reader = ArchiveReaderCache::Shared().Get(zip.path);
    FLOAR_REQUIRE(reader != nullptr);
    FLOAR_CHECK(&reader->Entries() == &reader->Entries());
    FLOAR_CHECK(reader->Entries().size() == 5);
    FLOAR_CHECK(reader->HasDirectory(L"Folder\\Sub"));
    FLOAR_CHECK(!reader->HasDirectory(L"root.txt"));
}

FLOAR_TEST(VfsArchive_ListDirectoryListsEachChildOnce)
{
    FixtureArchive zip;
    FLOAR_REQUIRE(zip.written);

    // "Folder/" is both an explicit entry and a parent: listed once.
    FLOAR_CHECK(SortedInnerPaths(VirtualFileSystem::ListDirectory(VirtualPath(zip.path))) ==
        (std::vector<std::wstring> { L"empty/", L"folder/", L"root.txt" }));
    FLOAR_CHECK(SortedInnerPaths(VirtualFileSystem::ListDirectory(VirtualPath(zip.path, L"FOLDER"))) ==
        (std::vector<std::wstring> { L"folder/b.txt", L"folder/sub/" }));
    FLOAR_CHECK(VirtualFileSystem::ListDirectory(VirtualPath(zip.path, L"Empty")).empty());
}
#endif
