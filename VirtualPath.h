#pragma once

#include <filesystem>
#include <string>
#include <optional>

namespace Floar
{
// Path policy (one rule for construction, equality, hashing and lookup):
//
// - Archive-inner paths are stored normalized: '\' becomes '/', repeated
//   separators collapse, and leading/trailing separators are dropped (case is
//   kept for display). They compare case-insensitively - towlower per UTF-16
//   unit, the same key the archive readers use for entry and directory lookup
//   (ArchiveInnerPathKey).
// - Host paths compare in lexical form (lexically_normal, preferred separators,
//   no trailing separator except at a root) and case-insensitively, as Windows
//   file systems do (HostPathKey). No file-system access is involved, so "." and
//   ".." are resolved lexically and relative paths stay relative.
// - std::hash<VirtualPath> hashes the same keys, so equal paths hash equally.
//
// The archive-reader cache keys archives by HostPathKey of the absolute path.

// '\' -> '/', collapsed separators, no leading or trailing '/'.
std::wstring NormalizeArchiveInnerPath(std::wstring path);
// Lookup key of an archive-inner path: normalized and lower-cased.
std::wstring ArchiveInnerPathKey(const std::wstring& path);
// Lookup key of a host path: lexical form, lower-cased.
std::wstring HostPathKey(const std::filesystem::path& path);

// VirtualPath represents a file location that can be either:
// 1. A regular file on disk: D:\textures\image.dds
// 2. A file inside an archive: D:\textures\pack.zip\folder\image.dds
struct VirtualPath
{
    std::filesystem::path hostPath;
    std::wstring archiveInnerPath;   // normalized (see the path policy above)

    VirtualPath() = default;

    VirtualPath(const std::filesystem::path& path)
        : hostPath(path)
    {
    }

    VirtualPath(const std::filesystem::path& archive, const std::wstring& innerPath)
        : hostPath(archive)
        , archiveInnerPath(NormalizeArchiveInnerPath(innerPath))
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
    // Same as VirtualFileSystem::Exists (archive readers come from its cache).
    bool Exists() const;

    bool operator==(const VirtualPath& other) const
    {
        return HostPathKey(hostPath) == HostPathKey(other.hostPath) &&
            ArchiveInnerPathKey(archiveInnerPath) == ArchiveInnerPathKey(other.archiveInnerPath);
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
        // Hashes the keys that operator== compares (see the path policy).
        size_t operator()(const Floar::VirtualPath& vp) const noexcept
        {
            const size_t h1 = hash<wstring>()(Floar::HostPathKey(vp.hostPath));
            const size_t h2 = hash<wstring>()(Floar::ArchiveInnerPathKey(vp.archiveInnerPath));
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }
    };
}
