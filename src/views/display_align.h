#pragma once

// The monitors placed to make the row, for as long as the game holds it, and put back at the leave. Windows' saved arrangement
// is never written, so a restart of the PC returns the player's own layout whatever happens here (views/display_move).
//
// Before anything moves, the record of the old places goes to disk and a guard process is running that waits on the game: the
// setup app in its guard mode, started once per game. Whichever way the game ends, a clean quit, a crash or a kill from outside,
// the guard reads the record and puts the monitors back. If the guard died too, the next start of the game or the app does.
// With no guard to be had, nothing moves: the row is refused, said in the log. Game thread only; each move and each return
// logged at Info, a refusal at Warning.
//
// Positions are in the desktop coordinates a DPI-aware process sees (the game's exe is per-monitor DPI aware), the same ones
// EnumDisplayMonitors gives the span.

#include "views/row_journal.h"

#include <span>
#include <string>

namespace lss::views::display_align {

/// A record left by an earlier run, undone before this one starts anything. Logged.
void HealEarlierRun() noexcept;

/// Places the monitors for as long as the row lasts and remembers where they were: each by its GDI device name as
/// GetMonitorInfo gives it (`\\.\DISPLAY2`), at its new top-left corner. False (logged) if the guard or the record could not
/// be had, a device is not an active source, or Windows refused; nothing is changed then.
bool Apply(std::span<const row_journal::Seen> places) noexcept;

/// Puts the monitors back, if the desktop still reads as this module left it (logged). Nothing happens when none is moved.
void Restore() noexcept;

/// A monitor is moved now.
bool Moved() noexcept;

/// The moved monitors, for the `window-span.state` reading; empty when none.
std::wstring Reading() noexcept;

}  // namespace lss::views::display_align
