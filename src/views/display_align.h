#pragma once

// A monitor of the row lined up with the row's top through Windows' display-configuration API (the one Display
// settings itself uses), for this Windows session only, and put back at the leave.
//
// Windows snaps a dragged monitor to the edges of its neighbours, so a pair placed beside a monitor of another size
// lands a few pixels off (the user's pair: 2 px, then 15 px, 2026-10-21), and the span needs the tops level or one
// column loses rows. The entry moves such a monitor itself (at most monitor_row::kAlignTolerance px): the active
// paths are queried, the source's position changed, the configuration applied with SetDisplayConfig without
// SDC_SAVE_TO_DATABASE, so the desktop changes for the session and Windows' saved arrangement does not. The leave
// puts every moved monitor back; so does the mod's unload. No display mode, resolution or scaling is changed, only
// a source's position. Game thread only; each move and each restore logged at Info, a refusal at Warning.
//
// Positions are in the desktop coordinates a DPI-aware process sees (the game's exe is per-monitor DPI aware), the
// same ones EnumDisplayMonitors gives the span [CONFIRMED live 2026-10-21: the TV read 3840x2160 in the mod's log].

#include <cstdint>
#include <span>
#include <string>

namespace lss::views::display_align {

/// One monitor to move: its GDI device name as GetMonitorInfo gives it (`\\.\DISPLAY2`) and the top edge to take.
struct Move
{
    std::wstring device;
    std::int32_t toTop = 0;
};

/// Moves each monitor for this Windows session and remembers where it was. False (logged) if any device is not an
/// active source or Windows refused the configuration; nothing is changed then.
bool Apply(std::span<const Move> moves) noexcept;

/// Puts every moved monitor back (logged). Nothing happens when none is moved.
void Restore() noexcept;

/// A monitor is moved now.
bool Moved() noexcept;

/// The moved monitors, for the `window-span.state` reading; empty when none.
std::wstring Reading() noexcept;

}  // namespace lss::views::display_align
