#pragma once

// A further player opens chests, picks up the shining items on the ground and opens doors with their own pad, as player 1
// does (research/RE-chest-two.md, §8 the pre-build review; Rebirth's player_two/field_pickup was the model).
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
//   - Only chests and ground items (an ObjectTreasure row), doors (Door_, HeavyDoor_) and the member's own swing
//     (TarzanJump_ and its tag) are pressed, the last two while the member's field actions run as the player's
//     (world/traversal_action.h). Talk and events stay player 1's: the player's prompt row is turned off whenever the
//     game shows one of those there.
//   - The press only queues the chest's open; the opener is taken a step later, when the open begins, as the party leader
//     at that moment. The chests a further player's press queued are noted, and that open's Begin runs in the player's
//     Leader scope, so the player's member opens it with the game's own animation (otherwise player 1's would walk over).
//   - The screen test of the pick (rows that need the target on screen) asks for "the first local player": inside a
//     further player's pick it answers with that player's.
// The open itself takes the game's field lock, which hands the party leader to its AI and holds every player for the
// open, as an opener is held in single player; the session hands the seats' members with it (players/session.h).
// The target icons stay player 1's: one widget per target, drawn in player 1's half only, so a further player's pick
// writes none (its icon state setter, its two direct state calls and its hold gauge pass while that pick runs) and never
// reaches the far-icon sweep (its copy's timer starts each run at zero). A further player's feedback is the prompt in
// their own command menu. Still shared, last run wins: the swing ropes' enter and exit switches.
// Logged: Info at install and placement; Debug per target change of a further player, per press, per chest noted and per
// open begun as a further player's; the `field.targets` reading (with the icon writes left out).

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::field_targets {

/// Checks the research's sites and functions, redirects the press call and the open's Begin. The redirects inside the
/// pick (three of its calls cross a block) are placed on the game thread by Tick. False (logged) if a check failed:
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
