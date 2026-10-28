#include "FileIdentity.h"

#include <cstring>

#include <Windows.h>

namespace Floar
{
namespace
{
    bool IsNotFound(DWORD error)
    {
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_FILENAME_EXCED_RANGE;
    }

    HANDLE OpenForQuery(const wchar_t* path)
    {
        // No data access: only attributes and IDs are read, and an open that
        // requests neither read, write nor delete access never conflicts with
        // another open's sharing mode.
        return CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
}

FileIdentity FileIdentityOfHandle(void* file)
{
    const HANDLE handle = static_cast<HANDLE>(file);
    BY_HANDLE_FILE_INFORMATION info {};
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return {};
    }
    FileIdentity identity;
    identity.known = true;
    identity.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    identity.writeTime = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;

    // The 128-bit ID where the file system has one (ReFS IDs do not fit the
    // 64-bit index); otherwise the 64-bit index. One file system always
    // answers the same way, so identities of one file compare equal.
    FILE_ID_INFO id {};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)))
    {
        identity.volume = id.VolumeSerialNumber;
        static_assert(sizeof(id.FileId) == sizeof(identity.fileId));
        std::memcpy(identity.fileId.data(), &id.FileId, sizeof(identity.fileId));
    }
    else
    {
        identity.volume = info.dwVolumeSerialNumber;
        const std::uint64_t index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        std::memcpy(identity.fileId.data(), &index, sizeof(index));
    }
    return identity;
}

FileIdentity FileIdentityOfPath(const std::filesystem::path& path, bool* missing)
{
    HANDLE file = OpenForQuery(path.c_str());
    DWORD error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (file == INVALID_HANDLE_VALUE && IsNotFound(error))
    {
        const std::wstring extended = ExtendedLengthPath(path.c_str());
        if (!extended.empty() && extended != path.native())
        {
            file = OpenForQuery(extended.c_str());
            if (file != INVALID_HANDLE_VALUE)
            {
                error = ERROR_SUCCESS;
            }
            else if (!IsNotFound(GetLastError()))
            {
                error = GetLastError();
            }
        }
    }
    if (missing)
    {
        *missing = file == INVALID_HANDLE_VALUE && IsNotFound(error);
    }
    if (file == INVALID_HANDLE_VALUE)
    {
        return {};
    }
    const FileIdentity identity = FileIdentityOfHandle(file);
    CloseHandle(file);
    return identity;
}

std::wstring ExtendedLengthPath(const wchar_t* path)
{
    const DWORD needed = GetFullPathNameW(path, 0, nullptr, nullptr);
    if (needed == 0)
    {
        return {};
    }
    std::wstring full(static_cast<size_t>(needed) + 3, L'\0');
    const DWORD length = GetFullPathNameW(path, static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (length == 0 || length >= full.size())
    {
        return {};
    }
    full.resize(length);

    if (full.starts_with(L"\\\\?\\"))
    {
        return full;   // already extended
    }
    if (full.starts_with(L"\\\\.\\"))
    {
        // A device path; \\.\C:\ names a drive like \\?\C:\ does.
        if (full.size() >= 7 && ((full[4] >= L'a' && full[4] <= L'z') || (full[4] >= L'A' && full[4] <= L'Z')) && full[5] == L':' && full[6] == L'\\')
        {
            full[2] = L'?';
            return full;
        }
        return {};
    }
    if (full.starts_with(L"\\\\"))
    {
        // \\server\share\... -> \\?\UNC\server\share\...
        const size_t share = full.find(L'\\', 2);
        if (share == std::wstring::npos || share == 2)
        {
            return {};
        }
        const size_t rest = full.find(L'\\', share + 1);
        if (rest == std::wstring::npos || rest == share + 1)
        {
            return {};
        }
        return L"\\\\?\\UNC\\" + full.substr(2);
    }
    return L"\\\\?\\" + full;
}
}
