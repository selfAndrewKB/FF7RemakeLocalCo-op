#pragma once

// The mod's own settings file: LocalSplitScreen.ini in the mod's folder (Mods/LocalSplitScreen/LocalSplitScreen.ini,
// one level above the dlls folder this module is loaded from), read with the Windows profile API, so a user sets a
// preset once without the console. A missing file or key gives the caller's default; the console switches still
// change everything at run time. Read on the game thread at install.

#include <string>

namespace lss::platform::settings {

/// The file's path, from this module's own path; empty if the module's path cannot be read.
std::wstring Path();

/// The key's text, trimmed and lower-cased; `fallback` when the file or the key is missing.
std::wstring Read(const wchar_t* section, const wchar_t* key, const wchar_t* fallback);

}  // namespace lss::platform::settings
