#pragma once

#include <cwctype>
#include <string>

namespace Floar
{
namespace StringUtil
{
    inline std::wstring ToLower(std::wstring value)
    {
        for (auto& ch : value)
        {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        return value;
    }

    inline std::wstring ToUpper(std::wstring value)
    {
        for (auto& ch : value)
        {
            ch = static_cast<wchar_t>(towupper(ch));
        }
        return value;
    }
}
}
