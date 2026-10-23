#pragma once

// Each seat's own battle target cursor: the game's cursor driver run again per seat (RE-target-cursor-two.md §6;
// Rebirth's hud/target_views kept the per-view opacity hide, its cursor logic was re-implemented there; the seats
// follow plan/seats.md step 3).
//
// The game has one battle target cursor, a widget whose pointer sits at `sys+0x10` of the battle system, outside the
// target fields each seat already has a copy of (battle/targeting_two.h). One function, the cursor driver, creates,
// moves, styles (the lock look, the dummy-lock flag), traces line of sight for and releases it, from the leader's menu
// target, lock, dummy lock or soft target, and writes the HUD's shared keys (the enemy bar's displayed target, the
// off-screen icon's name); the battle tick calls it right after each of its two targeting calls. So the cursor showed
// only when player 1 had a target, and player 2's half showed player 1's cursor at player 1's target through player
// 2's camera (the user's report, 2026-10-19). Now the driver runs again for each seat right after the seat's
// targeting run, inside the same scopes and the seat's target copy, with two more exchanges: the seat's cursor slot
// (its own widget, created by the game's own create) and its HUD keys. The line-of-sight step (once per frame from
// the field systems' tick) runs again for each seat's cursor with the same exchanges. The releases the game makes for
// player 1 alone (a battle suspension) release every seat's with the game's reflected HideBattleTargetIcon; the
// battle's end and a leave the same. In each column's pass of the HUD render every other player's cursor is
// transparent (the game's SetColorAndOpacity; a seat's column is its index), as the mod hides player 1's field
// cursors. The enemy bar's targeted look and the ability label in a seat's column follow the seat's key from here
// (hud/marker_replay classes the bars per column with DisplayedTarget). Not covered: the shared weak-point marker
// edges. Game thread only.

#include "hud/overlay_pass.h"

namespace lss::platform {
class GameImage;
}

namespace lss::battle::target_cursor_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: places the crossing and entry redirects once.
void Tick() noexcept;

/// Inside the seat's targeting run (its scopes and target copy open): the game's cursor driver for the seat.
void RunForSeat(int seat, void* system) noexcept;

/// The seat's cursor released with the game's own hide (the battle ended, its player left) and its copies dropped.
void End(int seat) noexcept;

/// The seat's displayed-target key from its copy of the enemy bar's HUD key (`object`, `controller`: a thread-safe
/// weak pair, both null when the seat has no target or no copy). For the HUD passes that classify the enemy bars
/// per column (hud/marker_replay).
bool DisplayedTarget(int seat, void*& object, void*& controller) noexcept;

/// Around each pass of the HUD render (the column whose pass runs, overlay_pass::kSingle for the game's one render):
/// every other player's cursor transparent.
void BeginPass(int column) noexcept;
void EndPass() noexcept;

}  // namespace lss::battle::target_cursor_two
