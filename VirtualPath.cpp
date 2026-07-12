#include "VirtualPath.h"
#include "ArchiveReader.h"
#include "StringUtil.h"
#include <algorithm>

namespace Floar
{
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
    if (IsInArchive())
    {
        auto reader = ArchiveReaderFactory::Open(hostPath);
        if (!reader)
        {
            return false;
        }
        return reader->HasEntry(archiveInnerPath);
    }
    else
    {
        return std::filesystem::exists(hostPath);
    }
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
                std::wstring innerPart = displayPath.substr(afterExt + 1);
                std::replace(innerPart.begin(), innerPart.end(), L'\\', L'/');
                return VirtualPath(archivePart, innerPart);
            }
            pos = lowerPath.find(ext, pos + 1);
        }
    }

    return VirtualPath(displayPath);
}
}
