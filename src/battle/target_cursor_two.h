#pragma once

// Player 2's own battle target cursor: the game's cursor driver run again for player 2 (RE-target-cursor-two.md §6;
// Rebirth's hud/target_views kept the per-view opacity hide, its cursor logic was re-implemented there).
//
// The game has one battle target cursor, a widget whose pointer sits at `sys+0x10` of the battle system, outside the
// target fields player 2 already has a copy of. One function, the cursor driver, creates, moves, styles (the lock
// look, the dummy-lock flag), traces line of sight for and releases it, from the leader's menu target, lock, dummy
// lock or soft target, and writes the HUD's shared keys (the enemy bar's displayed target, the off-screen icon's name);
// the battle tick calls it right after each of its two targeting calls. So the cursor showed only when player 1 had a
// target, and player 2's half showed player 1's cursor at player 1's target through player 2's camera (the user's
// report, 2026-10-19). Now the driver runs again for player 2 right after player 2's targeting run, inside the same
// scopes and the target copy, with two more exchanges: player 2's cursor slot (its own widget, created by the game's
// own create) and its HUD keys. The line-of-sight step (once per frame from the field systems' tick) runs again for
// player 2's cursor with the same exchanges. The releases the game makes for player 1 alone (a battle suspension)
// release player 2's with the game's reflected HideBattleTargetIcon; the battle's end and a leave the same. In each
// player's pass of the HUD render the other player's cursor is transparent (the game's SetColorAndOpacity), as the
// mod hides player 1's field cursors. Not covered: the enemy bar's targeted look and the ability label in player 2's
// half (the HUD ticks classify once with player 1's key), the shared weak-point marker edges. Game thread only.

#include "hud/overlay_pass.h"

namespace lss::platform {
class GameImage;
}

namespace lss::battle::target_cursor_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: places the crossing and entry redirects once.
void Tick() noexcept;

/// Inside player 2's targeting run (its scopes and target copy open): the game's cursor driver for player 2.
void RunForPlayerTwo(void* system) noexcept;

/// Player 2's cursor released with the game's own hide (the battle ended, player 2 left) and its copies dropped.
void End() noexcept;

/// Around each pass of the HUD render: the other player's cursor transparent.
void BeginPass(hud::overlay_pass::Pass pass) noexcept;
void EndPass() noexcept;

}  // namespace lss::battle::target_cursor_two
