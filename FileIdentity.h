#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Floar
{
// Identity of a file, for telling whether a path still names the file an
// archive reader scanned: its size and last-write time plus its stable file
// identifier (volume serial number and file ID). The file ID tells apart a
// replacement renamed over the archive even when it has the same size and
// write time. Internal to Floar (ArchiveReader, ArchiveReaderCache).
struct FileIdentity
{
    bool known { false };
    std::uint64_t size { 0 };
    std::uint64_t writeTime { 0 };            // FILETIME, 100 ns units
    std::uint64_t volume { 0 };               // volume serial number
    std::array<std::uint8_t, 16> fileId {};   // FILE_ID_128, or the 64-bit file index

    bool operator==(const FileIdentity&) const = default;
};

// Of an open file handle (a HANDLE); unknown when it cannot be queried.
FileIdentity FileIdentityOfHandle(void* file);

// Of the file `path` names: opened with no data access (sharing read, write
// and delete, so it never conflicts with other opens and the archive stays
// renamable) and closed at once. Unknown when it cannot be opened or queried
// (or names a directory); `missing` (optional) then tells whether the path
// was not found, as opposed to a file whose identity cannot be read. A path
// that is not found is retried in its extended-length form, so paths longer
// than MAX_PATH work.
FileIdentity FileIdentityOfPath(const std::filesystem::path& path, bool* missing = nullptr);

// The full path of `path` with the \\?\ (or \\?\UNC\) prefix, which lifts
// the MAX_PATH limit; empty when it has no such form. The same conversion
// as libarchive's __la_win_permissive_name_w.
std::wstring ExtendedLengthPath(const wchar_t* path);
}
