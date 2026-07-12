#pragma once

#include <filesystem>
#include <string>
#include <optional>

namespace Floar
{
// VirtualPath represents a file location that can be either:
// 1. A regular file on disk: D:\textures\image.dds
// 2. A file inside an archive: D:\textures\pack.zip\folder\image.dds
struct VirtualPath
{
    std::filesystem::path hostPath;
    std::wstring archiveInnerPath;

    VirtualPath() = default;

    VirtualPath(const std::filesystem::path& path)
        : hostPath(path)
    {
    }

    VirtualPath(const std::filesystem::path& archive, const std::wstring& innerPath)
        : hostPath(archive)
        , archiveInnerPath(innerPath)
    {
    }

    bool IsInArchive() const
    {
        return !archiveInnerPath.empty();
    }

    bool empty() const
    {
        return hostPath.empty();
    }

    bool IsFilesystemPath() const
    {
        return archiveInnerPath.empty();
    }

    std::wstring wstring() const
    {
        return GetDisplayPath();
    }

    std::filesystem::path filename() const
    {
        return std::filesystem::path(GetFilename());
    }

    bool IsArchiveFile() const;

    std::wstring GetDisplayPath() const
    {
        if (IsInArchive())
        {
            return hostPath.wstring() + L"\\" + archiveInnerPath;
        }
        return hostPath.wstring();
    }

    std::wstring GetFilename() const
    {
        if (IsInArchive())
        {
            auto pos = archiveInnerPath.find_last_of(L"/\\");
            if (pos != std::wstring::npos)
            {
                return archiveInnerPath.substr(pos + 1);
            }
            return archiveInnerPath;
        }
        return hostPath.filename().wstring();
    }

    std::wstring GetExtension() const
    {
        std::wstring name = GetFilename();
        auto pos = name.find_last_of(L'.');
        if (pos != std::wstring::npos)
        {
            return name.substr(pos);
        }
        return L"";
    }

    VirtualPath GetParent() const;
    bool Exists() const;

    bool operator==(const VirtualPath& other) const
    {
        return hostPath == other.hostPath && archiveInnerPath == other.archiveInnerPath;
    }

    bool operator!=(const VirtualPath& other) const
    {
        return !(*this == other);
    }

    static VirtualPath FromFilesystem(const std::filesystem::path& path)
    {
        return VirtualPath(path);
    }

    static VirtualPath FromArchive(const std::filesystem::path& archivePath, const std::wstring& innerPath)
    {
        return VirtualPath(archivePath, innerPath);
    }

    static std::optional<VirtualPath> Parse(const std::wstring& displayPath);
};
}

namespace std
{
    template<>
    struct hash<Floar::VirtualPath>
    {
        size_t operator()(const Floar::VirtualPath& vp) const noexcept
        {
            size_t h1 = hash<wstring>()(vp.hostPath.wstring());
            size_t h2 = hash<wstring>()(vp.archiveInnerPath);
            return h1 ^ (h2 << 1);
        }
    };
}
