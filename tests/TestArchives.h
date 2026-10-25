#pragma once

// Test fixtures shared by the Floar test files.

#include "FloarLog.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#if FLOAR_ENABLE_COMMON_ARCHIVES
#include "archive.h"
#include "archive_entry.h"

namespace FloarTest
{
using Floar::Log::ToUtf8;

// Writes a ZIP with the given files (name -> data) and explicit directory entries.
inline bool WriteZip(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::wstring, std::string>>& files,
    const std::vector<std::wstring>& directories)
{
    struct archive* a = archive_write_new();
    if (!a)
    {
        return false;
    }
    bool ok = archive_write_set_format_zip(a) == ARCHIVE_OK &&
        archive_write_set_options(a, "zip:hdrcharset=UTF-8") == ARCHIVE_OK &&
        archive_write_open_filename_w(a, path.c_str()) == ARCHIVE_OK;
    for (const auto& name : directories)
    {
        if (!ok)
        {
            break;
        }
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, ToUtf8(name).c_str());
        archive_entry_set_filetype(entry, AE_IFDIR);
        archive_entry_set_perm(entry, 0755);
        ok = archive_write_header(a, entry) == ARCHIVE_OK;
        archive_entry_free(entry);
    }
    for (const auto& [name, data] : files)
    {
        if (!ok)
        {
            break;
        }
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, ToUtf8(name).c_str());
        archive_entry_set_size(entry, static_cast<la_int64_t>(data.size()));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        ok = archive_write_header(a, entry) == ARCHIVE_OK &&
            archive_write_data(a, data.data(), data.size()) == static_cast<la_ssize_t>(data.size());
        archive_entry_free(entry);
    }
    ok = (archive_write_close(a) == ARCHIVE_OK) && ok;
    archive_write_free(a);
    return ok;
}

// Writes a solid 7z (libarchive's 7-Zip writer stores every file in one LZMA2
// stream, so reading an entry decompresses every entry before it).
inline bool WriteSolid7z(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::wstring, std::string>>& files,
    int compressionLevel = 1)
{
    struct archive* a = archive_write_new();
    if (!a)
    {
        return false;
    }
    const std::string level = std::to_string(compressionLevel);
    bool ok = archive_write_set_format_7zip(a) == ARCHIVE_OK &&
        archive_write_set_format_option(a, "7zip", "compression", "lzma2") == ARCHIVE_OK &&
        archive_write_set_format_option(a, "7zip", "compression-level", level.c_str()) == ARCHIVE_OK &&
        archive_write_open_filename_w(a, path.c_str()) == ARCHIVE_OK;
    for (const auto& [name, data] : files)
    {
        if (!ok)
        {
            break;
        }
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, ToUtf8(name).c_str());
        archive_entry_set_size(entry, static_cast<la_int64_t>(data.size()));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        ok = archive_write_header(a, entry) == ARCHIVE_OK &&
            archive_write_data(a, data.data(), data.size()) == static_cast<la_ssize_t>(data.size());
        archive_entry_free(entry);
    }
    ok = (archive_write_close(a) == ARCHIVE_OK) && ok;
    archive_write_free(a);
    return ok;
}

}
#endif
