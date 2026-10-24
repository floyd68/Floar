#include "TestHarness.h"

#include "ArchiveReader.h"
#include "FloarLog.h"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#if FLOAR_ENABLE_COMMON_ARCHIVES
#include "archive.h"
#include "archive_entry.h"
#endif

// Floar logging: laziness without a sink, UTF-8 conversion, and that opening
// and extracting an archive whose path is outside the active code page cannot
// fail because of logging (with a recording sink, a throwing sink, or none).

namespace
{
    using namespace Floar;

    std::mutex g_sinkMutex;
    std::vector<std::string> g_messages;

    void RecordingSink(Log::Level, const std::string& message)
    {
        std::lock_guard<std::mutex> lock(g_sinkMutex);
        g_messages.push_back(message);
    }

    void ThrowingSink(Log::Level, const std::string&)
    {
        throw std::runtime_error("sink failure");
    }

    struct ScopedSink
    {
        explicit ScopedSink(Log::Sink sink)
        {
            {
                std::lock_guard<std::mutex> lock(g_sinkMutex);
                g_messages.clear();
            }
            Log::SetSink(sink);
        }
        ~ScopedSink()
        {
            Log::SetSink(nullptr);
        }
    };

    int g_evaluations = 0;

    int CountedArgument()
    {
        ++g_evaluations;
        return 42;
    }

    // Emoji (surrogate pair), Japanese, and Thai: outside every single-byte or
    // East Asian ANSI code page at once.
    const std::wstring kNonAcpStem = L"\U0001F600_テスト_ไทย";
    const std::string kNonAcpStemUtf8 =
        "\xF0\x9F\x98\x80_\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88_\xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2";
}

FLOAR_TEST(Log_ArgumentsNotEvaluatedWithoutSink)
{
    Log::SetSink(nullptr);
    g_evaluations = 0;
    FLOAR_LOG_ERROR("value {}", CountedArgument());
    FLOAR_LOG_WARN("value {}", CountedArgument());
    FLOAR_LOG_INFO("value {}", CountedArgument());
    FLOAR_CHECK(g_evaluations == 0);

    ScopedSink sink(&RecordingSink);
    FLOAR_LOG_ERROR("value {}", CountedArgument());
    FLOAR_CHECK(g_evaluations == 1);
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    FLOAR_REQUIRE(g_messages.size() == 1);
    FLOAR_CHECK(g_messages[0] == "value 42");
}

FLOAR_TEST(Log_ToUtf8ConvertsNonAcpText)
{
    FLOAR_CHECK(Log::ToUtf8(std::wstring_view(kNonAcpStem)) == kNonAcpStemUtf8);
    FLOAR_CHECK(Log::ToUtf8(std::filesystem::path(kNonAcpStem)) == kNonAcpStemUtf8);
    FLOAR_CHECK(Log::ToUtf8(std::wstring_view()).empty());

    // An unpaired surrogate does not throw; it becomes U+FFFD.
    const std::wstring lone = L"a\xD800" L"b";
    FLOAR_CHECK(Log::ToUtf8(std::wstring_view(lone)) == "a\xEF\xBF\xBD" "b");
}

#if FLOAR_ENABLE_COMMON_ARCHIVES
namespace
{
    const std::string kEntryName = "inner.txt";
    const std::string kEntryData = "hello from a non-ACP archive";

    // Writes a ZIP with one entry at `path` (wide path; libarchive's _w open).
    bool WriteZip(const std::filesystem::path& path)
    {
        struct archive* a = archive_write_new();
        if (!a)
        {
            return false;
        }
        bool ok = archive_write_set_format_zip(a) == ARCHIVE_OK &&
            archive_write_open_filename_w(a, path.c_str()) == ARCHIVE_OK;
        if (ok)
        {
            struct archive_entry* entry = archive_entry_new();
            archive_entry_set_pathname(entry, kEntryName.c_str());
            archive_entry_set_size(entry, static_cast<la_int64_t>(kEntryData.size()));
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            ok = archive_write_header(a, entry) == ARCHIVE_OK &&
                archive_write_data(a, kEntryData.data(), kEntryData.size()) == static_cast<la_ssize_t>(kEntryData.size());
            archive_entry_free(entry);
        }
        ok = (archive_write_close(a) == ARCHIVE_OK) && ok;
        archive_write_free(a);
        return ok;
    }

    std::filesystem::path NonAcpZipPath()
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        return std::filesystem::path(tempPath) /
            (L"FloarTests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + kNonAcpStem + L".zip");
    }

    struct ScopedZip
    {
        std::filesystem::path path = NonAcpZipPath();
        bool written = WriteZip(path);
        ~ScopedZip()
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    };

    // Opens the archive, lists it, and extracts the entry; true when all succeed.
    bool OpenListExtract(const std::filesystem::path& path)
    {
        auto reader = ArchiveReaderFactory::Open(path);
        if (!reader)
        {
            return false;
        }
        const auto entries = reader->ListEntries();
        if (entries.size() != 1 || entries[0].name != L"inner.txt")
        {
            return false;
        }
        const auto bytes = reader->ExtractToMemory(entries[0].name);
        return std::string(bytes.begin(), bytes.end()) == kEntryData;
    }
}

FLOAR_TEST(Log_NonAcpArchiveOpensAndLogsUtf8WithSink)
{
    ScopedZip zip;
    FLOAR_REQUIRE(zip.written);

    ScopedSink sink(&RecordingSink);
    FLOAR_CHECK(OpenListExtract(zip.path));

    std::lock_guard<std::mutex> lock(g_sinkMutex);
    bool found = false;
    for (const auto& message : g_messages)
    {
        if (message.find("[LibArchiveReader] Opening: ") != std::string::npos &&
            message.find(kNonAcpStemUtf8) != std::string::npos)
        {
            found = true;
        }
    }
    FLOAR_CHECK(found);
}

FLOAR_TEST(Log_NonAcpArchiveWorksWithoutSink)
{
    ScopedZip zip;
    FLOAR_REQUIRE(zip.written);
    Log::SetSink(nullptr);
    FLOAR_CHECK(OpenListExtract(zip.path));
}

FLOAR_TEST(Log_ThrowingSinkCannotBreakArchiveAccess)
{
    ScopedZip zip;
    FLOAR_REQUIRE(zip.written);
    ScopedSink sink(&ThrowingSink);
    FLOAR_CHECK(OpenListExtract(zip.path));
}
#endif
