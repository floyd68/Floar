#include "VirtualPath.h"
#include "ArchiveReader.h"
#include "StringUtil.h"
#include "VirtualFileSystem.h"
#include <algorithm>

namespace Floar
{
std::wstring NormalizeArchiveInnerPath(std::wstring path)
{
    std::wstring out;
    out.reserve(path.size());
    for (wchar_t ch : path)
    {
        if (ch == L'\\')
        {
            ch = L'/';
        }
        if (ch == L'/' && (out.empty() || out.back() == L'/'))
        {
            continue;  // leading or repeated separator
        }
        out.push_back(ch);
    }
    if (!out.empty() && out.back() == L'/')
    {
        out.pop_back();
    }
    return out;
}

std::wstring ArchiveInnerPathKey(const std::wstring& path)
{
    return StringUtil::ToLower(NormalizeArchiveInnerPath(path));
}

std::wstring HostPathKey(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {};
    }
    std::wstring text = path.lexically_normal().make_preferred().wstring();
    // "C:\dir\" -> "C:\dir", but keep roots such as "C:\" or "\\".
    const size_t rootLength = path.lexically_normal().make_preferred().root_path().wstring().size();
    while (text.size() > rootLength && (text.back() == L'\\' || text.back() == L'/'))
    {
        text.pop_back();
    }
    return StringUtil::ToLower(std::move(text));
}

bool VirtualPath::IsArchiveFile() const
{
    if (IsInArchive())
    {
        return false;
    }
    return ArchiveReaderFactory::IsArchiveFile(hostPath);
}

VirtualPath VirtualPath::GetParent() const
{
    if (IsInArchive())
    {
        auto pos = archiveInnerPath.find_last_of(L"/\\");
        if (pos != std::wstring::npos && pos > 0)
        {
            return VirtualPath(hostPath, archiveInnerPath.substr(0, pos));
        }
        else
        {
            return VirtualPath(hostPath);
        }
    }
    else
    {
        if (hostPath.has_parent_path())
        {
            return VirtualPath(hostPath.parent_path());
        }
        return *this;
    }
}

bool VirtualPath::Exists() const
{
    return VirtualFileSystem::Exists(*this);
}

std::optional<VirtualPath> VirtualPath::Parse(const std::wstring& displayPath)
{
    if (displayPath.empty())
    {
        return std::nullopt;
    }

    std::wstring lowerPath = StringUtil::ToLower(displayPath);
    std::vector<std::wstring> archiveExts = ArchiveReaderFactory::GetSupportedExtensions();

    for (const auto& ext : archiveExts)
    {
        size_t pos = lowerPath.find(ext);
        while (pos != std::wstring::npos)
        {
            size_t afterExt = pos + ext.length();
            if (afterExt < displayPath.length() &&
                (displayPath[afterExt] == L'\\' || displayPath[afterExt] == L'/'))
            {
                std::wstring archivePart = displayPath.substr(0, afterExt);
                // The constructor normalizes the inner part.
                return VirtualPath(archivePart, displayPath.substr(afterExt + 1));
            }
            pos = lowerPath.find(ext, pos + 1);
        }
    }

    return VirtualPath(displayPath);
}
}
