#include "views/monitor_list.h"

#include <cwctype>
#include <map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::views::monitor_list {

namespace {

BOOL CALLBACK Add(HMONITOR handle, HDC, LPRECT, LPARAM param) noexcept
try
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info))
    {
        return TRUE;
    }
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const auto getDpi = reinterpret_cast<GetDpiForMonitorFn>(
        reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor")));
    if (getDpi != nullptr)
    {
        getDpi(handle, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY);
    }
    Listed listed;
    listed.monitor = {{info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom}, dpiX};
    listed.device  = info.szDevice;
    listed.handle  = handle;
    reinterpret_cast<std::vector<Listed>*>(param)->push_back(std::move(listed));
    return TRUE;
}
catch (...)
{
    return FALSE;
}

std::wstring Lower(std::wstring text)
{
    for (wchar_t& c : text)
    {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return text;
}

struct Identity
{
    std::wstring path, name;
};

/// Each active output by its GDI device name; the first target of a cloned output stands for it.
std::map<std::wstring, Identity> Identities()
{
    std::map<std::wstring, Identity>       identities;
    std::vector<DISPLAYCONFIG_PATH_INFO>   paths;
    std::vector<DISPLAYCONFIG_MODE_INFO>   modes;
    LONG                                   result = ERROR_INSUFFICIENT_BUFFER;
    while (result == ERROR_INSUFFICIENT_BUFFER)
    {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        {
            return identities;
        }
        paths.resize(pathCount);
        modes.resize(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_SUCCESS)
        {
            paths.resize(pathCount);
        }
    }
    if (result != ERROR_SUCCESS)
    {
        return identities;
    }
    for (const DISPLAYCONFIG_PATH_INFO& path : paths)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size      = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id        = path.sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size      = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id        = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
        {
            identities.try_emplace(source.viewGdiDeviceName, Identity{target.monitorDevicePath, target.monitorFriendlyDeviceName});
        }
    }
    return identities;
}

}  // namespace

std::vector<Listed> List()
{
    std::vector<Listed> list;
    EnumDisplayMonitors(nullptr, nullptr, Add, reinterpret_cast<LPARAM>(&list));
    const auto identities = Identities();
    for (Listed& listed : list)
    {
        const auto found = identities.find(listed.device);
        const bool known = found != identities.end();
        listed.id        = Lower(known && !found->second.path.empty() ? found->second.path : listed.device);
        listed.name      = known && !found->second.name.empty() ? found->second.name : listed.device;
        listed.key       = known ? monitor_row::IdentityKey(listed.id, found->second.name) : std::wstring();
        // Shown to the player: a monitor may pad the name it reports with spaces.
        listed.name.erase(listed.name.find_last_not_of(L" \t\r\n") + 1);
    }
    return list;
}

}  // namespace lss::views::monitor_list
