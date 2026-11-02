#pragma once

// One option's word written into the settings file (platform/settings.h) with the Windows profile API, the API the
// file is read with: the key's line is replaced as `key=word`, a key or section the file lacks is added (the section at
// the end of the file, the key at the end of its section), and every other line, comments included, stays as written.
// The file is created when there is none. Inline so the host test writes a real file (tests/settings_test.cpp).

#include "platform/settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::platform::settings {

/// Writes `word` for `option` into the file at `path`. 0 when written, else the Windows error.
inline unsigned long WriteWord(const wchar_t* path, const Option& option, const wchar_t* word) noexcept
{
    if (WritePrivateProfileStringW(option.section, option.key, word, path) == 0)
    {
        const unsigned long error = GetLastError();
        return error != 0 ? error : ERROR_WRITE_FAULT;
    }
    return 0;
}

}  // namespace lss::platform::settings
