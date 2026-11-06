#include "platform/settings.h"

#include "platform/log.h"
#include "platform/settings_write.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <optional>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::platform::settings {

namespace {

// Written by Load on the game thread at start, and by Save (the Co-op page's choices, game thread).
std::array<Reading, kOptions.size()> g_readings = [] {
    std::array<Reading, kOptions.size()> defaults;
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        defaults[i] = {kOptions[i]->fallback, Source::Default, {}};
    }
    return defaults;
}();

std::optional<std::size_t> IndexOf(const Option& option) noexcept
{
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        if (kOptions[i] == &option)
        {
            return i;
        }
    }
    return std::nullopt;
}

/// The keys the file holds that are no row of the table ("[Views] old-key"), comma-separated; empty for none.
std::wstring KeysNotInTable(const std::wstring& path)
{
    const auto list = [](const wchar_t* section, const std::wstring& file) {
        std::wstring buffer(16384, L'\0');
        const DWORD  length = section != nullptr
                                  ? GetPrivateProfileStringW(section, nullptr, L"", buffer.data(), static_cast<DWORD>(buffer.size()), file.c_str())
                                  : GetPrivateProfileSectionNamesW(buffer.data(), static_cast<DWORD>(buffer.size()), file.c_str());
        std::vector<std::wstring> names;
        for (std::size_t at = 0; at < length && buffer[at] != L'\0'; at += names.back().size() + 1)
        {
            names.emplace_back(buffer.c_str() + at);
        }
        return names;
    };
    std::wstring stale;
    for (const std::wstring& section : list(nullptr, path))
    {
        for (const std::wstring& key : list(section.c_str(), path))
        {
            const bool known = std::any_of(kOptions.begin(), kOptions.end(), [&](const Option* o) {
                return _wcsicmp(o->section, section.c_str()) == 0 && _wcsicmp(o->key, key.c_str()) == 0;
            });
            if (!known)
            {
                stale += std::format(L"{}[{}] {}", stale.empty() ? L"" : L", ", section, key);
            }
        }
    }
    return stale;
}

const wchar_t* SourceWord(Source source) noexcept
{
    switch (source)
    {
        case Source::File:
            return L"the file";
        case Source::Invalid:
            return L"the default; the file's word is not valid";
        case Source::Default:
        default:
            return L"the default";
    }
}

}  // namespace

std::wstring Path()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&Path), &module))
    {
        return {};
    }
    wchar_t    buffer[MAX_PATH * 2]{};
    const auto length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
    {
        return {};
    }
    try
    {
        // <mod>/dlls/main.dll -> <mod>/LocalSplitScreen.ini
        return (std::filesystem::path(buffer).parent_path().parent_path() / kFileName).wstring();
    }
    catch (...)
    {
        return {};
    }
}

void Load()
{
    const std::wstring path   = Path();
    const bool         exists = !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::wstring       summary;
    g_readings = ReadFile(path);
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        const Option& option = *kOptions[i];
        if (g_readings[i].source == Source::Invalid)
        {
            LogWarning(L"settings: {}", InvalidText(option, g_readings[i]));
        }
        summary += std::format(L"{}{} = {} ({})", i == 0 ? L"" : L", ", option.key, g_readings[i].word, SourceWord(g_readings[i].source));
    }
    if (exists)
    {
        Log(L"settings: {} read: {}", path, summary);
    }
    else
    {
        Log(L"settings: no {} ({}); every option at its default: {}", kFileName, path.empty() ? L"the module's path could not be read" : path, summary);
    }
    if (path.empty())
    {
        return;
    }
    // The file written from the table, the values kept: every option described as the code has it now.
    const std::wstring stale   = exists ? KeysNotInTable(path) : std::wstring();
    bool               written = false;
    if (const unsigned long error = WriteFromTable(path, g_readings, &written); error != 0)
    {
        LogWarning(L"settings: {} could not be written from the mod's options (Windows error {}); it is read as it is", path,
                   error);
    }
    else if (written)
    {
        Log(L"settings: {} written from the mod's options, the values kept{}", path,
            stale.empty() ? std::wstring() : std::format(L"; dropped, no longer the mod's: {}", stale));
    }
    else
    {
        LogDebug(L"settings: {} already reads as the mod's options", path);
    }
}

const Reading& Of(const Option& option) noexcept
{
    if (const auto index = IndexOf(option))
    {
        return g_readings[*index];
    }
    static Reading unlisted;  // one at a time is enough for a programming error that Listed's static_assert catches
    static bool    logged = false;
    if (!logged)
    {
        logged = true;
        LogWarning(L"settings: [{}] {} is not a row of the table; its default {} is used and the file is not read for it",
                   option.section, option.key, option.fallback);
    }
    unlisted = {option.fallback, Source::Default, {}};
    return unlisted;
}

std::wstring Why(const Option& option)
{
    return WhyText(option, Of(option));
}

bool Save(const Option& option, const std::wstring& word)
{
    const auto    index   = IndexOf(option);
    const Reading judged  = Judge(option, word);
    if (!index || judged.source != Source::File)
    {
        LogWarning(L"settings: [{}] {} = '{}' was not saved (not an option's word)", option.section, option.key, word);
        return false;
    }
    const std::wstring path = Path();
    if (path.empty())
    {
        LogWarning(L"settings: [{}] {} = {} was not saved (the module's path could not be read)", option.section, option.key,
                   judged.word);
        return false;
    }
    auto readings    = g_readings;
    readings[*index] = judged;
    bool written     = false;
    if (const unsigned long error = WriteFromTable(path, readings, &written); error != 0)
    {
        LogWarning(L"settings: [{}] {} = {} was not saved to {} (Windows error {})", option.section, option.key, judged.word,
                   path, error);
        return false;
    }
    g_readings = readings;
    Log(L"settings: [{}] {} = {} saved to {}", option.section, option.key, judged.word, path);
    return true;
}

}  // namespace lss::platform::settings
