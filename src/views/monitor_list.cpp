#include "views/monitor_list.h"

#include "platform/text.h"
#include "views/display_config.h"

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

struct Identity
{
    std::wstring id, name;  // display_config::MonitorId, and the name the monitor reports (empty if unread)
};

/// Each active output by its GDI device name; the first target of a cloned output stands for it.
std::map<std::wstring, Identity> Identities()
{
    std::map<std::wstring, Identity> identities;
    const auto                       config = display_config::Query(QDC_ONLY_ACTIVE_PATHS);
    if (!config)
    {
        return identities;
    }
    for (const DISPLAYCONFIG_PATH_INFO& path : config->paths)
    {
        const std::wstring source = display_config::SourceName(path);
        if (!source.empty())
        {
            const auto target = display_config::TargetOf(path);
            identities.try_emplace(source, Identity{display_config::MonitorId(path, target), target ? target->name : std::wstring()});
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
        listed.id        = known ? found->second.id : platform::Lower(listed.device);
        listed.name      = known && !found->second.name.empty() ? found->second.name : listed.device;
        listed.key       = known ? monitor_picks::IdentityKey(listed.id, found->second.name) : std::wstring();
        // Shown to the player: a monitor may pad the name it reports with spaces.
        listed.name.erase(listed.name.find_last_not_of(L" \t\r\n") + 1);
    }
    return list;
}

}  // namespace lss::views::monitor_list
