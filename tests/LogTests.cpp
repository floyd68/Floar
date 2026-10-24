#include "TestHarness.h"

#include "ArchiveReader.h"
#include "FloarLog.h"

#include <windows.h>

#include <clocale>
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

    bool IsValidUtf8(const std::string& text)
    {
        return text.empty() ||
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
    }

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

FLOAR_TEST(Log_CodePageToUtf8DecodesNarrowText)
{
    FLOAR_CHECK(Log::CodePageToUtf8("plain ascii", 932) == "plain ascii");
    // Latin-1 0xE9 and Shift-JIS "テスト".
    FLOAR_CHECK(Log::CodePageToUtf8("caf\xE9", 28591) == "caf\xC3\xA9");
    FLOAR_CHECK(Log::CodePageToUtf8("\x83\x65\x83\x58\x83\x67", 932) == "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88");
    // Undecodable bytes still produce valid UTF-8.
    FLOAR_CHECK(IsValidUtf8(Log::CodePageToUtf8("bad \xFF\xFE utf8", CP_UTF8)));
}

#if FLOAR_ENABLE_COMMON_ARCHIVES
namespace
{
    struct FixtureEntry
    {
        std::wstring name;
        std::string data;
    };

    // One ASCII entry and one whose name is outside every ANSI code page.
    const std::vector<FixtureEntry> kEntries = {
        { L"inner.txt", "hello from a non-ACP archive" },
        { kNonAcpStem + L".txt", "non-ACP entry payload" },
    };

    // Writes a ZIP holding kEntries at `path` (wide path; libarchive's _w open),
    // storing entry names as UTF-8.
    bool WriteZip(const std::filesystem::path& path)
    {
        struct archive* a = archive_write_new();
        if (!a)
        {
            return false;
        }
        bool ok = archive_write_set_format_zip(a) == ARCHIVE_OK &&
            archive_write_set_options(a, "zip:hdrcharset=UTF-8") == ARCHIVE_OK &&
            archive_write_open_filename_w(a, path.c_str()) == ARCHIVE_OK;
        for (const auto& fixture : kEntries)
        {
            if (!ok)
            {
                break;
            }
            struct archive_entry* entry = archive_entry_new();
            archive_entry_set_pathname_utf8(entry, Log::ToUtf8(fixture.name).c_str());
            archive_entry_set_size(entry, static_cast<la_int64_t>(fixture.data.size()));
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            ok = archive_write_header(a, entry) == ARCHIVE_OK &&
                archive_write_data(a, fixture.data.data(), fixture.data.size()) == static_cast<la_ssize_t>(fixture.data.size());
            archive_entry_free(entry);
        }
        ok = (archive_write_close(a) == ARCHIVE_OK) && ok;
        archive_write_free(a);
        return ok;
    }

    std::filesystem::path TempTestPath(const std::wstring& stem)
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        return std::filesystem::path(tempPath) /
            (L"FloarTests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + stem + L".zip");
    }

    struct ScopedZip
    {
        std::filesystem::path path = TempTestPath(kNonAcpStem);
        bool written = WriteZip(path);
        ~ScopedZip()
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    };

    // Opens the archive, lists it, and extracts every entry; true when all succeed.
    bool OpenListExtract(const std::filesystem::path& path)
    {
        auto reader = ArchiveReaderFactory::Open(path);
        if (!reader)
        {
            return false;
        }
        const auto entries = reader->ListEntries();
        if (entries.size() != kEntries.size())
        {
            return false;
        }
        for (const auto& fixture : kEntries)
        {
            bool listed = false;
            for (const auto& entry : entries)
            {
                listed = listed || entry.name == fixture.name;
            }
            if (!listed)
            {
                return false;
            }
            const auto bytes = reader->ExtractToMemory(fixture.name);
            if (std::string(bytes.begin(), bytes.end()) != fixture.data)
            {
                return false;
            }
        }
        return true;
    }

    bool AnyMessageContains(const std::string& prefix, const std::string& text)
    {
        for (const auto& message : g_messages)
        {
            if (message.find(prefix) != std::string::npos && message.find(text) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    // Sets the CRT LC_CTYPE locale for the scope and restores it afterwards.
    struct ScopedCtypeLocale
    {
        std::string saved;
        bool applied = false;
        explicit ScopedCtypeLocale(const char* locale)
        {
            if (const char* current = setlocale(LC_CTYPE, nullptr))
            {
                saved = current;
            }
            applied = setlocale(LC_CTYPE, locale) != nullptr;
        }
        ~ScopedCtypeLocale()
        {
            setlocale(LC_CTYPE, saved.empty() ? "C" : saved.c_str());
        }
    };

    // An existing .zip that libarchive cannot open (held with no sharing), so
    // its "Failed to open '%ls'" diagnostic carries the non-ASCII path.
    struct LockedFile
    {
        std::filesystem::path path;
        HANDLE handle = INVALID_HANDLE_VALUE;
        explicit LockedFile(std::filesystem::path filePath)
            : path(std::move(filePath))
        {
            handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        ~LockedFile()
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    };

    // Opens a locked file whose name contains Latin-1 and Japanese text and
    // returns libarchive's part of the logged "Failed to open" error.
    std::string LoggedOpenDiagnostic()
    {
        LockedFile locked(TempTestPath(L"err_é_テスト"));
        if (locked.handle == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        ScopedSink sink(&RecordingSink);
        ArchiveReaderFactory::Open(locked.path);

        std::lock_guard<std::mutex> lock(g_sinkMutex);
        for (const auto& message : g_messages)
        {
            if (message.find("[LibArchiveReader] ERROR: Failed to open ") == std::string::npos)
            {
                continue;
            }
            const size_t diagnostic = message.rfind("Failed to open '");
            if (diagnostic != std::string::npos && diagnostic > 0)
            {
                return message.substr(diagnostic);
            }
        }
        return {};
    }
}

FLOAR_TEST(Log_NonAcpArchiveOpensAndLogsUtf8WithSink)
{
    ScopedZip zip;
    FLOAR_REQUIRE(zip.written);

    ScopedSink sink(&RecordingSink);
    FLOAR_CHECK(OpenListExtract(zip.path));

    std::lock_guard<std::mutex> lock(g_sinkMutex);
    FLOAR_CHECK(AnyMessageContains("[LibArchiveReader] Opening: ", kNonAcpStemUtf8));
#ifdef _DEBUG
    FLOAR_CHECK(AnyMessageContains("[LibArchiveReader] ExtractToMemory: ", kNonAcpStemUtf8 + ".txt"));
#endif
    for (const auto& message : g_messages)
    {
        FLOAR_CHECK(IsValidUtf8(message));
    }
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

FLOAR_TEST(Log_LibArchiveDiagnosticIsUtf8InCLocale)
{
    // "C" locale: libarchive writes U+00E9 as the single byte 0xE9.
    ScopedCtypeLocale locale("C");
    FLOAR_REQUIRE(locale.applied);
    const std::string diagnostic = LoggedOpenDiagnostic();
    FLOAR_REQUIRE(!diagnostic.empty());
    FLOAR_CHECK(IsValidUtf8(diagnostic));
    FLOAR_CHECK(diagnostic.find("err_\xC3\xA9_") != std::string::npos);
}

FLOAR_TEST(Log_LibArchiveDiagnosticIsUtf8InShiftJisLocale)
{
    // Code page 932 locale: libarchive writes the Japanese as Shift-JIS bytes.
    ScopedCtypeLocale locale(".932");
    FLOAR_REQUIRE(locale.applied);
    const std::string diagnostic = LoggedOpenDiagnostic();
    FLOAR_REQUIRE(!diagnostic.empty());
    FLOAR_CHECK(IsValidUtf8(diagnostic));
    FLOAR_CHECK(diagnostic.find("\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88") != std::string::npos);
}
#endif
