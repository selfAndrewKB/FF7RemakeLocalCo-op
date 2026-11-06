#pragma once

// Which monitors a spanned game window covers: a row, one view per monitor.
//
// A window that spans several monitors shows one view per monitor if the monitors touch, side by side, with their tops level.
// Each view is its monitor's own size (views/column_rules ColumnRecords), so the choice lists the widths and heights. The window
// is as tall as the tallest: below a shorter monitor its bottom falls off the desktop, where nothing is seen, and nothing else
// may stand there. Windows scaling does not matter to a picked row: the game's exe is per-monitor DPI aware, so it draws in real
// pixels on every monitor, and any resize the engine makes on a DPI change is caught by the span's re-apply. With nothing picked (Choose) a
// row is still of one height with no scaling: the player has not asked for that row by name, so only the plain case is taken.
// This module decides from a list of monitors; it touches no OS and no engine, so it is unit-tested on the host
// (tests/monitor_row_test.cpp).
//
// Two ways to a row:
//   - Choose, nothing picked: a row the desktop already has. Every row on the desktop counts, the game's own monitor's
//     first, else the longest. A monitor a few pixels off the row's top is listed as a move, since Windows snaps a dragged
//     monitor to its neighbours and a pair beside a monitor of another size lands a few pixels off every time. The row's top
//     is that of a member attached to the rest of the desktop: moved off an attached monitor's top, Windows drags the
//     others along to keep them on it and the desktop never reads as the row.
//   - Arrange, the player's picks: any monitors, in the order picked, left to right, wherever they sit. The moves make the
//     row for as long as the game holds it (views/display_align); Windows' own arrangement is never changed. Monitors not
//     picked stay where they are unless the row needs the place or their only link to the desktop went with it.
// Which of the two a settings file gets, and how its picks are found on the desktop, is views/monitor_picks.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lss::views::monitor_row {

/// A monitor this many pixels (or fewer) off the row's top is lined up by the span when nothing is picked; further off, it
/// is no row. Picked monitors move any distance.
constexpr std::int32_t kAlignTolerance = 32;

/// The widest view seen clean in the game: 4000 px is, 4200 px has a band of wrong colours at its right side, wider at 5120 px,
/// with or without Windows scaling. 4001 to 4199 px is unmeasured and the cause is not known; 4096 is a guess at a texture limit.
constexpr std::int32_t kWidestCleanView = 4096;

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

/// A monitor to place before the window is: `from` as Windows has it, to the given top-left corner.
struct Move
{
    Rect         from;
    std::int32_t toLeft = 0;
    std::int32_t toTop  = 0;
    bool         operator==(const Move&) const noexcept = default;
};

struct Choice
{
    bool              ok          = false;
    Rect              span;             // the window rect to take, when ok (with the moves made)
    std::int32_t      rowMonitors = 0;  // monitors in the row: for Choose, of one height, within the tolerance, touching, unscaled; for Arrange, the picks
    bool              gamesRow    = false;  // the chosen row holds the game's monitor
    std::vector<Move> moves;            // when ok: the monitors to place first (usually none)
    std::vector<std::int32_t> widths;   // when ok: the span's monitors' widths, left to right (one per column)
    std::vector<std::int32_t> heights;  // when ok: their heights, the same way; the span is as tall as the tallest
    std::wstring      why;              // when not ok: what is missing or wrong, and what to change in Windows
    std::wstring      stopped;          // when not ok: only what to change in Windows; empty if no monitor stopped a row
};

/// `columns` >= 1 monitors of a row the desktop has; the row around `monitors[gameMonitor]` when it qualifies.
Choice Choose(std::span<const Monitor> monitors, std::size_t gameMonitor, std::int32_t columns);

/// A row of exactly `picks`, indices into `monitors` in order left to right, wherever they sit now: the moves make it. The
/// primary monitor (the one at 0,0) never moves, since Windows ties it to that corner; the desktop stays one connected
/// piece with no overlap, because Windows ignores any other layout without a word; no other monitor stands in the box the
/// window takes; among the layouts that do, the one that moves the fewest monitors wins, so a row that is already there has
/// no moves at all.
Choice Arrange(std::span<const Monitor> monitors, std::span<const std::size_t> picks);

}  // namespace lss::views::monitor_row
