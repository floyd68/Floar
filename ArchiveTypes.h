#pragma once

#include <string>
#include <vector>

#ifndef FLOAR_ENABLE_COMMON_ARCHIVES
#define FLOAR_ENABLE_COMMON_ARCHIVES 1
#endif

namespace Floar
{
namespace ArchiveTypes
{
    inline bool IsBethesdaArchiveExt(const std::wstring& extLower)
    {
        return extLower == L".bsa" || extLower == L".ba2";
    }

    inline bool IsCommonArchiveExt(const std::wstring& extLower)
    {
        return extLower == L".zip" || extLower == L".7z" || extLower == L".rar";
    }

    inline bool IsKnownArchiveExt(const std::wstring& extLower)
    {
        return IsBethesdaArchiveExt(extLower) || IsCommonArchiveExt(extLower);
    }

    inline const std::vector<std::wstring>& SupportedReadExtensions()
    {
        static const std::vector<std::wstring> exts = {
#if FLOAR_ENABLE_COMMON_ARCHIVES
            L".zip", L".7z", L".rar",
#endif
            L".ba2"
        };
        return exts;
    }
}
}
