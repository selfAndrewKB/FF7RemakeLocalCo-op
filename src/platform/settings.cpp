#include "platform/settings.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::platform::settings {

namespace {

constexpr const wchar_t* kFileName = L"LocalSplitScreen.ini";

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

std::wstring Read(const wchar_t* section, const wchar_t* key, const wchar_t* fallback)
{
    const std::wstring path = Path();
    wchar_t            buffer[256]{};
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return fallback;
    }
    const auto length = GetPrivateProfileStringW(section, key, fallback, buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    std::wstring value(buffer, length);
    const auto   notSpace = [](wchar_t c) { return !std::iswspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

}  // namespace lss::platform::settings
