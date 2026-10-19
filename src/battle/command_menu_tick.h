#pragma once

// Player 2's command controller ticked at the game's own spot, inside player 2's scopes (RE-battle.md §4,
// RE-command-menu-two.md §7-§8, RE-battle-member-two.md §2.6; Rebirth's player_two/command_menu_tick).
//
// The game ticks its one command controller once per frame from the field systems' tick (one call, crossing a
// code block: it is redirected from the game thread, outside that function). The game's controller ticks as
// before; then player 2's (battle/command_menu_two.h) ticks inside player 2's scopes (players/scopes.h) with
// pad 2's buttons and sticks, so its poll reads its own widget, and its sub-ticks call the game's per-character
// attack, guard, dodge and command functions on player 2's member. Right after, the game's leader-only input
// buffer (combo follow-ups, dodge retries, buffered uniques) is run for player 2's member with the same call the
// game makes for the leader, under the game's own gates.
//
// The tick's one write of the world's battle-command speed slot (the menu's time slow) is redirected: player 1's
// value is noted and player 2's tick writes the smaller of the two, so either menu slows the world. Pad 2's key
// events reach player 2's widgets through player 2's own focus context, opened around each event once player
// 2's menu is up in that context. Nothing of player 1's gates the route: player 2's context holds player 2's
// widgets alone, and a modal UI (the pause menu, a tutorial) stops both controllers' polls inside the game's own
// tick gates. (Until v0.20.0 the route also needed player 1's top focus layer to be its own command menu, so every
// layer player 1 pushed above it, its Tactical Mode list or its target list, cut pad 2 off from player 2's menu:
// the user's "player 2 could not dodge or open Tactical Mode" while player 1's leader lay KO'd with a list open.)
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::command_menu_tick {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: places the crossing redirects once.
void Tick() noexcept;

/// Which player's controller is ticking now (0 none, 1 the game's, 2 player 2's).
int TickingPlayer() noexcept;
/// True while one of pad 2's key events runs in player 2's context.
bool EventContextActive() noexcept;

}  // namespace lss::battle::command_menu_tick
