#pragma once

// A further player opens chests, picks up the shining items on the ground, opens doors and curtains and fires the
// grappling hook with their own pad, as player 1 does (research/RE-chest-two.md, §8 the pre-build review;
// research/RE-hookshot-two.md; Rebirth's player_two/field_pickup was the model).
//
// The game keeps one field target manager. Each frame its pick finds the nearest target around the party leader (the
// prompt row, the icons, the leader's glance), and its press opens the current target on FieldAction. Both are one game
// call, so each runs a second time for each further player right after the game's own: inside that player's Leader,
// Primary and UiSlot scopes, with the player's own copy of the manager's per-player state exchanged in (the current
// target, its prompt text and hold timer, the last pick's visited entries and trace map, the FieldAction bytes, the sweep
// timer, and each entry's "was the current target" byte). The game's own code then picks around the player's member,
// writes the prompt into the player's own command menu, and presses for the player's member.
//   - The press's FieldAction read is inline in its caller (a tick of thirty systems that cannot run twice), so for a
//     further player the mod makes it with the game's own gate and getters inside the pad's button scope and writes the
//     three bytes the game writes (held, pressed, the latch cleared on release): the one rule restated here.
//   - Only chests and ground items (an ObjectTreasure row), doors (Door_, HeavyDoor_), curtains (Curtain_, an
//     experiment), grappling-hook points (HookShot_) and the member's own swing (TarzanJump_ and its tag) are pressed,
//     all but chests while the member's field actions run as the player's (world/traversal_action.h). Talk, events and
//     object events stay player 1's: for the player's pick every other active target is marked disabled (the entry's
//     own byte, the pick's rejection test; put back after, unless the run applied the game's own pending write to it),
//     so the game gives the player neither an icon nor a prompt for them.
//   - While the player's member runs a field action, the player's pick sees the game's "no new action" switch on as the
//     game's leader's action sets it (world/traversal_action.h NoActionScope): every target dimmed, nothing to press, so
//     a second press cannot restart the running action (a restarted grappling hook loses its animations). Player 1's own
//     action dims the further players' targets as before.
//   - The press only queues the chest's open; the opener is taken a step later, when the open begins, as the party leader
//     at that moment. The chests a further player's press queued are noted, and that open's Begin runs in the player's
//     Leader scope, so the player's member opens it with the game's own animation (otherwise player 1's would walk over).
//   - The screen test of the pick (rows that need the target on screen) asks for "the first local player": inside a
//     further player's pick it answers with that player's.
// The open itself takes the game's field lock, which hands the party leader to its AI and holds every player for the
// open, as an opener is held in single player; the session hands the seats' members with it (players/session.h).
// The target icons are each player's own (world/target_icons.h): the player's widgets are exchanged in around its pick
// and press, so the game makes, updates and sweeps them, and each view shows only its player's. Still shared, last run
// wins: the swing ropes' enter and exit switches.
// Logged: Info at install and placement; Debug per target change of a further player, per press, per chest noted and per
// open begun as a further player's; the `field.targets` reading (with each player's icon counts).

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::field_targets {

/// Checks the research's sites and functions, redirects the press call and the open's Begin. The redirects inside the
/// pick (two of them cross a block) are placed on the game thread by Tick. False (logged) if a check failed:
/// further players then have no prompts or presses of their own.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick: the seat's local player and member while its player is in (null otherwise).
void TickSeat(int seat, void* localPlayer, void* member) noexcept;

/// Game thread, each engine tick after the seats.
void Tick() noexcept;

void Remove() noexcept;

/// The `field.targets` reading.
std::wstring Reading() noexcept;

}  // namespace lss::world::field_targets
