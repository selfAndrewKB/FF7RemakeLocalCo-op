#include "platform/settings.h"

#include "platform/log.h"

#include <filesystem>
#include <format>
#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::platform::settings {

namespace {

constexpr const wchar_t* kFileName = L"LocalSplitScreen.ini";

// Written by Load on the game thread at start, then only read.
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
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        const Option& option = *kOptions[i];
        std::wstring  text;
        if (exists)
        {
            wchar_t    buffer[256]{};
            const auto length = GetPrivateProfileStringW(option.section, option.key, L"", buffer,
                                                         static_cast<DWORD>(std::size(buffer)), path.c_str());
            text.assign(buffer, length);
        }
        g_readings[i] = Judge(option, text);
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

}  // namespace lss::platform::settings
