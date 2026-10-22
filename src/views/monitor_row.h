#pragma once

// Which monitors a spanned game window may cover: a row of equal monitors.
//
// A window that spans several monitors shows one view per monitor only if every monitor it covers has the same
// pixel size, the same top edge and no gap to its neighbour, and if none of them is scaled by Windows (the game's
// exe is per-monitor DPI aware and the engine resizes the window itself on a DPI change; research
// RE-multi-monitor-views.md §1, §4). This module decides that from a list of monitors and the index of the game's
// one; it touches no OS and no engine, so it is unit-tested on the host (tests/monitor_row_test.cpp).
//
// Taken from Rebirth's views/monitor_row. Changed: the row need not contain the game's monitor. Remake's entry
// moves the window through the game's own resolution request and the engine's reshape, so a row of equal monitors
// anywhere on the desktop qualifies (the user plays on an ultrawide above a pair of equal monitors). The game's own
// row is preferred when it qualifies; otherwise the longest qualifying row. Within the row the span takes `columns`
// monitors starting at the game's own when it is in the row (shifted left as far as needed when the row ends
// before that), else from the row's left. Seat-indexed by design: each column is one seated player's view, so a
// two-player span covers two monitors and leaves a third idle.

#include <cstdint>
#include <span>
#include <string>

namespace lss::views::monitor_row {

struct Rect
{
    std::int32_t left   = 0;
    std::int32_t top    = 0;
    std::int32_t right  = 0;
    std::int32_t bottom = 0;

    std::int32_t Width() const noexcept { return right - left; }
    std::int32_t Height() const noexcept { return bottom - top; }
    bool         operator==(const Rect&) const noexcept = default;
};

struct Monitor
{
    Rect          rect;
    std::uint32_t dpi = 96;  // 96 = 100 % scaling
};

struct Choice
{
    bool         ok          = false;
    Rect         span;             // the window rect to take, when ok
    std::int32_t rowMonitors = 0;  // monitors in the chosen row (equal, aligned, touching, unscaled)
    bool         gamesRow    = false;  // the chosen row holds the game's monitor
    std::wstring why;              // when not ok: what is missing or wrong
};

/// `columns` >= 1 monitors of a row of equal monitors; the row around `monitors[gameMonitor]` when it qualifies.
Choice Choose(std::span<const Monitor> monitors, std::size_t gameMonitor, std::int32_t columns);

}  // namespace lss::views::monitor_row
