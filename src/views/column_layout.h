#pragma once

// Three equal columns through the engine's own layout table (plan/seats.md step 5; research/RE-three-views.md).
//
// UGameViewportClient lays its local players out every redraw from its own layout table (SplitscreenInfo, filled by
// its constructor): with three local players it takes the three-player entry the settings name, ThreePlayer_FavorTop
// by default, whose records put player 1 on top and the other two in the quadrants below (research/RE-layout-fov.md
// §2, RE-multi-monitor-views.md §7). UE 4.18 has no three-column entry (Rebirth's 4.26 has one, which its third view
// selects by the setting; read 2026-10-22, cleanup-2026-10 views/third_view). So while three columns are wanted on a
// span of monitors, or asked for on the console (in one window three players keep the engine's own entry, player 1
// across the top and two below: the user's pick of 2026-10-24), this module writes that entry's three records to three
// equal full-height columns (or the stock records arranged by the column order, below), in place, and puts the stock
// records back otherwise and at the mod's end. The layout
// code itself stays the engine's: it reads the table on every redraw (nothing is cached), so the write takes effect on
// the next frame and the restore likewise. The entry
// is written only when it holds the stock records or one of the mod's (any other content is logged once and left
// alone, the hold released), and restored only while it still holds one of the mod's; a failed read or write is logged once and retried
// twice a second. The three-player layout setting is held at FavorTop (the entry written) while the mod's records are in,
// as the span holds the two-player one at side by side; the hold is by hand here as there (Rebirth's generic
// held setting, read 2026-10-22, was not ported: two bytes held in two modules do not need it). [A data write into
// the engine's layout table, not a recreation of the layout: the research's one documented lever for three columns.]
//
// The columns a frame shows are one number for the mod (views/column_rules.h): the players in, the span's columns
// while held, or the console's override `view-columns 0..3` (0: as the span and the players decide). mod.cpp passes
// that number here and to the mirror player each tick.
//
// The column order (the settings file's `column-order` under [Views], the console's `column-order 123 | 132`; the
// user 2026-10-25): which screen players 2 and 3 take when three views show, whatever the order they joined in. The
// entry's records are arranged for it (column_rules::Slots): `1 3 2` puts player 2's view on the right (in one window,
// three players: bottom right) and player 3's in the middle (bottom left); a mirror takes a screen left over. Player 1
// keeps the left (top). The join order (`1 2 3`, the default) arranges nothing: the views keep the engine's list order,
// and in one window the stock entry stays untouched, as before. Under `1 3 2` in one window the mod writes the arranged
// top-and-two-below entry and holds the three-player setting at player 1 on top, as it does for the columns. The mod's columns stay the views' places in the engine's list; only where they show moves.
// Reading `column-layout.state`. Game thread only. Logged at Info on each write and restore and on an order change,
// at Debug for the status.

#include "views/column_rules.h"

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::views::column_layout {

/// Finds GEngine and registers the commands. False (logged) if the engine global is outside the image.
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, after the span's: `columns` the columns wanted this frame (views/column_rules::Wanted);
/// `localPlayers` the local players the engine lists (player 1, the seats in, the mirrors); `spanColumns` the span's
/// columns (0 when out); `seats` whose view each place of the engine's list holds now. Three columns while three views
/// exist (wanted, or three local players still listed) and the window spans monitors or the console asks for them
/// (column_rules::ThreeColumns); in one window three players take the engine's own layout, player 1 across the top and
/// two below. Either is arranged by the column order while three views exist.
void Tick(int columns, int localPlayers, int spanColumns, const column_rules::ListSeats& seats) noexcept;

/// The console's override (`view-columns`): 0 for none.
int Override() noexcept;

/// The column order now (the setting, the console, the pad claim).
column_rules::Order CurrentOrder() noexcept;

/// Whether the last Tick placed three views by the column order (columns or local players three): the claim places its
/// screens by the same test.
bool ThreeViews() noexcept;

/// Sets the column order (logged with `why`); the next Tick arranges the entry. Game thread (players/pad_claim).
void ChangeOrder(const column_rules::Order& order, const wchar_t* why) noexcept;

/// Puts the stock records and the setting back if the mod's are in. At the mod's end.
void Remove() noexcept;

std::wstring Reading();

}  // namespace lss::views::column_layout
