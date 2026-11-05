#pragma once

// The desktop's monitors as Windows has them now, for the mod's span (views/window_span) and the setup app (setup/):
// each monitor's rect and effective DPI, its GDI device name, and an id and a name from the display configuration. The
// id is the monitor's device path, lower-cased: unlike the DISPLAYn number it stays with the monitor when Windows
// renumbers its outputs, so it is what the settings file names. A process needs per-monitor DPI awareness for these
// rects and DPIs to be real (the game's exe declares it, the setup app sets it). Not on the game thread only: it keeps
// no state.

#include "views/monitor_row.h"

#include <string>
#include <vector>

namespace lss::views::monitor_list {

struct Listed
{
    monitor_row::Monitor monitor;
    std::wstring         device;  // "\\.\DISPLAY2", as the display APIs take it
    std::wstring         id;      // the device path, lower-cased; the device name lower-cased if the path is unreadable
    std::wstring         name;    // the monitor's own name ("DELL AW3423DW"); the device name if it has none
    void*                handle = nullptr;  // its HMONITOR
};

std::vector<Listed> List();

}  // namespace lss::views::monitor_list
