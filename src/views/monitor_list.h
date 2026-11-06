#pragma once

// The desktop's monitors as Windows has them now, for the mod's span (views/window_span) and the setup app (setup/):
// each monitor's rect and effective DPI, its GDI device name, and an id and a name from the display configuration. The
// id is the monitor's device path, lower-cased: unlike the DISPLAYn number it stays with the monitor when Windows
// renumbers its outputs, so it is what the settings file names, with the identity that finds a monitor whose path changed
// (monitor_picks::IdentityKey). A process needs per-monitor DPI awareness for these
// rects and DPIs to be real (the game's exe declares it, the setup app sets it). Not on the game thread only: it keeps
// no state.

#include "views/monitor_picks.h"
#include "views/monitor_row.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lss::views::monitor_list {

struct Listed
{
    monitor_row::Monitor monitor;
    std::wstring         device;  // "\\.\DISPLAY2", as the display APIs take it
    std::wstring         id;      // the device path, lower-cased; the device name lower-cased if the path is unreadable
    std::wstring         name;    // the monitor's own name ("DELL AW3423DW"); the device name if it has none
    std::wstring         key;     // its model and name, monitor_picks::IdentityKey; empty when the path is unreadable
    void*                handle = nullptr;  // its HMONITOR
};

std::vector<Listed> List();

/// The list as the row rules take it: each monitor's rect and DPI, in the list's order.
inline std::vector<monitor_row::Monitor> Plain(std::span<const Listed> listed)
{
    std::vector<monitor_row::Monitor> monitors;
    for (const Listed& m : listed)
    {
        monitors.push_back(m.monitor);
    }
    return monitors;
}

/// The monitors a settings word names, found in the list (monitor_picks::FindPicks): its `found` holds indices into the list.
inline monitor_picks::Picks PicksIn(std::span<const Listed> listed, std::wstring_view word)
{
    std::vector<std::wstring> ids, keys;
    for (const Listed& m : listed)
    {
        ids.push_back(m.id);
        keys.push_back(m.key);
    }
    return monitor_picks::FindPicks(ids, keys, word);
}

}  // namespace lss::views::monitor_list
