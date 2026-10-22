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
// Changed again (v0.38.0): a monitor whose top is within kAlignTolerance of the row's is in the row, read as if it
// sat at the row's top (the top of the row's leftmost monitor); the choice lists the moves that make that true,
// and the span makes them in Windows before placing the window (views/display_align). Windows snaps a dragged
// monitor to its neighbours' edges, so a pair beside a monitor of another size lands a few pixels off every time
// (the user's pair: 2 px, then 15 px); asking the user to fix that by hand failed twice in one evening.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lss::views::monitor_row {

/// A monitor this many pixels (or fewer) off the row's top is lined up by the span; further off, it is refused.
constexpr std::int32_t kAlignTolerance = 32;

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

/// A monitor of the span to shift vertically before the window is placed: `from` as Windows has it, to `toTop`.
struct Move
{
    Rect         from;
    std::int32_t toTop = 0;
    bool         operator==(const Move&) const noexcept = default;
};

struct Choice
{
    bool              ok          = false;
    Rect              span;             // the window rect to take, when ok (with the moves made)
    std::int32_t      rowMonitors = 0;  // monitors in the chosen row (equal, within the tolerance, touching, unscaled)
    bool              gamesRow    = false;  // the chosen row holds the game's monitor
    std::vector<Move> moves;            // when ok: the monitors of the span to line up first (usually none)
    std::wstring      why;              // when not ok: what is missing or wrong, and what to change in Windows
};

/// `columns` >= 1 monitors of a row of equal monitors; the row around `monitors[gameMonitor]` when it qualifies.
Choice Choose(std::span<const Monitor> monitors, std::size_t gameMonitor, std::int32_t columns);

}  // namespace lss::views::monitor_row
