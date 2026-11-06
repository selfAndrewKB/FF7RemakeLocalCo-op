#pragma once

// Small text helpers with no home of their own.

#include <cwctype>
#include <string>

namespace lss::platform {

/// `text` in lower case, letter by letter (std::towlower).
inline std::wstring Lower(std::wstring text)
{
    for (wchar_t& c : text)
    {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return text;
}

}  // namespace lss::platform
