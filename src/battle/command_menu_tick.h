#pragma once

// Each seat's command controller ticked at the game's own spot, inside the seat's scopes (RE-battle.md §4,
// RE-command-menu-two.md §7-§8, RE-battle-member-two.md §2.6; Rebirth's player_two/command_menu_tick, which stayed
// single-seat; the seats follow plan/seats.md step 3).
//
// The game ticks its one command controller once per frame from the field systems' tick (one call, crossing a
// code block: it is redirected from the game thread, outside that function). The game's controller ticks as
// before; then each built seat's (battle/command_menu_two.h) ticks inside that seat's scopes (players/scopes.h)
// with its own pad's buttons and sticks (a seat's pad is XInput user `seat`), so its poll reads its own widget,
// and its sub-ticks call the game's per-character attack, guard, dodge and command functions on the seat's
// member. Right after, the game's leader-only input buffer (combo follow-ups, dodge retries, buffered uniques) is
// run for the seat's member with the same call the game makes for the leader, under the game's own gates.
//
// The tick's one write of the world's battle-command speed slot (the menu's time slow, written by every tick) is
// redirected: a seat's tick writes the least of the values the menus asked for so far this frame
// (battle/command_menu_rules.h; the last write wins, so the frame ends on the least), so any menu slows the world.
// A seat's pad's key events reach the seat's widgets through the seat's own focus context, opened around each
// event once the seat's menu is up in that context. Nothing of player 1's gates the
// route: a seat's context holds the seat's widgets alone, and a modal UI (the pause menu, a tutorial) stops every
// controller's poll inside the game's own tick gates. (Until v0.20.0 the route also needed player 1's top focus
// layer to be its own command menu, so every layer player 1 pushed above it, its Tactical Mode list or its target
// list, cut pad 2 off from player 2's menu: the user's "player 2 could not dodge or open Tactical Mode" while
// player 1's leader lay KO'd with a list open.)
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::command_menu_tick {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: starts the slow rule's frame; places the crossing redirect once.
void Tick() noexcept;

/// True while a command controller ticks at the game's spot (a seat's input buffer included); TickingSeat() then
/// names whose: 0 player 1's (the game's), `s` seat s's. 0 outside any tick as well.
bool Ticking() noexcept;
int  TickingSeat() noexcept;
/// The seat whose pad's key event runs inside that seat's context now, else 0.
int EventSeat() noexcept;

}  // namespace lss::battle::command_menu_tick
