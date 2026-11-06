#pragma once

// Windows' display configuration as QueryDisplayConfig gives it: the outputs (paths) and their modes, with the names that go
// with a path. Read by views/display_move, for the monitors' positions, and views/monitor_list, for each monitor's device path
// and name. The header carries Windows' own types, so only those two include it. No state.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <optional>
#include <string>
#include <vector>

namespace lss::views::display_config {

struct Config
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

/// The configuration for `flags`: QDC_ONLY_ACTIVE_PATHS for the desktop now, QDC_DATABASE_CURRENT for the arrangement Windows
/// has saved for the connected monitors. Nothing if Windows gives none.
std::optional<Config> Query(UINT32 flags);

/// The GDI device name of a path's source ("\\.\DISPLAY2"); empty if it could not be read.
std::wstring SourceName(const DISPLAYCONFIG_PATH_INFO& path);

/// The monitor at a path's far end: its device path and the name it reports.
struct Target
{
    std::wstring path, name;
};

/// Nothing if it could not be read.
std::optional<Target> TargetOf(const DISPLAYCONFIG_PATH_INFO& path);

/// The id of a path's monitor, given its target as TargetOf read it: the device path, lower-cased. With no path to go by,
/// the source's name, lower-cased: such an id is only as stable as Windows' numbering. One function for everyone
/// who names a monitor, so the settings file, the list and the record agree.
std::wstring MonitorId(const DISPLAYCONFIG_PATH_INFO& path, const std::optional<Target>& target);

/// The id is a device path, not a source's name standing in for one.
bool HasPath(const std::optional<Target>& target) noexcept;

/// A path's source mode, which holds its desktop position; null when the path has none.
const DISPLAYCONFIG_SOURCE_MODE* SourceMode(const Config& config, const DISPLAYCONFIG_PATH_INFO& path) noexcept;
DISPLAYCONFIG_SOURCE_MODE*       SourceMode(Config& config, const DISPLAYCONFIG_PATH_INFO& path) noexcept;

}  // namespace lss::views::display_config
