#pragma once

// Three columns through the engine's own layout table (plan/seats.md step 5; research/RE-three-views.md); on a span of
// monitors of different widths each column is its own monitor's width (v0.147.0).
//
// UGameViewportClient lays its local players out every redraw from its own layout table (SplitscreenInfo, filled by
// its constructor): with three local players it takes the three-player entry the settings name, ThreePlayer_FavorTop
// by default, whose records put player 1 on top and the other two in the quadrants below (research/RE-layout-fov.md
// §2, RE-multi-monitor-views.md §7). UE 4.18 has no three-column entry (Rebirth's 4.26 has one, which its third view
// selects by the setting; read 2026-10-22, cleanup-2026-10 views/third_view). So while three columns are wanted on a
// span of monitors, or asked for on the console, this module writes that entry's three records to full-height columns;
// in one window it writes the layout the setting `three-player-split` names (column_rules::ThreeSplitRecords: one
// across the top and two below, the engine's own entry and the default, one across the bottom, or one down either side).
// Either is arranged by the column order (below), in place, and the stock
// records go back otherwise and at the mod's end. The layout
// code itself stays the engine's: it reads the table on every redraw (nothing is cached), so the write takes effect on
// the next frame and the restore likewise. The entry
// is written only when it holds the stock records or one of the mod's (any other content is logged once and left
// alone, the hold released), and restored only while it still holds one of the mod's; a failed read or write is logged once and retried
// twice a second. The three-player layout setting is held at FavorTop (the entry written) while the mod's records are in.
// The two-player one is held at side by side while the window spans monitors or the setting `two-player-split` asks for
// it, and put back otherwise (the setting's `horizontal` and `top` are the game's own values, not forced); the engine
// re-reads both every layout pass. [A data write into
// the engine's layout table, not a recreation of the layout: the research's one documented lever for three columns.]
//
// The columns a frame shows are one number for the mod (views/column_rules.h): the players in, the span's columns
// while held, or the console's override `view-columns 0..3` (0: as the span and the players decide). mod.cpp passes
// that number here and to the mirror player each tick.
//
// The column order (the settings file's `column-order` under [Views], the console's `column-order 123 | 132 | 213 ...`;
// the user 2026-10-25, every player from 2026-10-31): which screen each player takes, by player number, whatever the
// order they joined in. The entry's records are arranged for it (column_rules::Slots): `1 3 2` puts player 2's view on
// the right (in one window, three players: the last of the other two) and player 3's in the middle, `2 1 3` player 2's
// first (the large view) and player 1's second; a mirror takes a screen left over. With the players listed as the order
// puts them (`1 2 3`, the default, and players 2 and 3 joined in that order) and the default layout the arrangement is the
// stock entry, untouched in one window. Otherwise in one window the mod writes the arranged layout and holds the
// three-player setting at FavorTop, as it does for the columns. Two views take the engine's two-player entries (top and bottom, side by side), both kept the
// same way: the two players in the order's order (RE-layout-fov.md §9). The mod's columns stay the views' places in the
// engine's list; only where they show moves.
// Reading `column-layout.state`. Game thread only. Logged at Info on each write and restore and on an order change,
// at Debug for the status.

#include "views/column_rules.h"

#include <cstdint>
#include <span>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::views::column_layout {

/// Finds GEngine and registers the commands. False (logged) if the engine global is outside the image.
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, after the span's: `columns` the columns wanted this frame (views/column_rules::Wanted);
/// `localPlayers` the local players the engine lists (player 1, the seats in, the mirrors); `spanColumns` the span's
/// columns (0 when out) and `spanWidths` its monitors' widths, left to right; `seats` whose view each place of the
/// engine's list holds now. Three columns while three views exist (wanted, or three local players still listed) and the
/// window spans monitors or the console asks for them (column_rules::ThreeColumns); in one window three players take the
/// setting's layout. Either is arranged by the column order while three views exist. On a span each column is its own monitor's width (column_rules::ColumnRecords; two players on a two-monitor
/// span likewise, through the side-by-side entry); the console's three columns in one window are equal.
void Tick(int columns, int localPlayers, int spanColumns, std::span<const std::int32_t> spanWidths,
          const column_rules::ListSeats& seats) noexcept;

/// The console's override (`view-columns`): 0 for none.
int Override() noexcept;

/// The column order now (the setting, the console, the player windows).
column_rules::Order CurrentOrder() noexcept;

/// The views the last Tick placed by the column order: 3 (columns or local players three), 2 (two), else 1. The player
/// windows place their screens by the same count (column_rules::ScreenSlots).
int Views() noexcept;

/// Sets the column order (logged with `why`); the next Tick arranges the entry. Game thread (players/player_window).
void ChangeOrder(const column_rules::Order& order, const wchar_t* why) noexcept;

/// Puts the stock records and the setting back if the mod's are in. At the mod's end.
void Remove() noexcept;

std::wstring Reading();

}  // namespace lss::views::column_layout
