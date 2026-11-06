#pragma once

// The desktop as the span (views/window_span) reads it: every monitor, which one holds the game window, and the monitors the
// settings file names found among them; with the row those give (views/monitor_picks) and the moves that make it, by the
// ids the display calls take. No state. The shape is cheap enough to ask twice a second; the full read asks Windows
// for its display configuration and is made only at an entry, on a change of shape and while monitors are being moved.

#include "views/monitor_picks.h"
#include "views/monitor_row.h"
#include "views/row_journal.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct HWND__;

namespace lss::views::span_desktop {

struct Desktop
{
    std::vector<monitor_row::Monitor> list;     // every monitor: its rect and effective DPI
    std::vector<std::wstring>         devices;  // their GDI device names, the same way: for the log
    std::vector<std::wstring>         ids;      // their ids (monitor_list), the same way: what the display calls and the record name them by
    std::size_t                       gameAt = 0;  // the game window's monitor; list.size() when it is not listed
    monitor_picks::Picks              picks;    // the settings file's monitors found among them
};

/// The desktop's shape: every monitor's rect, hashed, so a monitor coming, going or moving changes it, even one that leaves
/// the desktop's outer bounds as they were. No device names and no display configuration are read.
std::uint64_t Shape() noexcept;

/// The desktop now. Empty when it cannot be read: a row is then refused, with the row's own reason.
Desktop Read(HWND__* game) noexcept;

/// The row of `columns` columns the settings file's monitors give on this desktop (monitor_picks::Row).
monitor_row::Choice Row(const Desktop& desktop, std::int32_t columns);

/// The row with the most columns, up to `most` (monitor_picks::WidestRow).
monitor_row::Choice WidestRow(const Desktop& desktop, std::int32_t most);

/// A choice's moves as the display calls take them: each monitor by the id its rect was listed with, and its new
/// top-left corner. Not as many as the choice's moves when a monitor to move is not in the list, or two listed monitors share
/// a rect.
std::vector<row_journal::Seen> PlacesFor(const monitor_row::Choice& choice, const Desktop& desktop);

/// Every monitor as read, for the log: device, rect, DPI, the game's marked.
std::wstring Text(const Desktop& desktop);

}  // namespace lss::views::span_desktop
