#pragma once

// Players 2 and 3 work the game's levers and switches with their own pad, as player 1 does
// (research/RE-further-player-levers.md): a push-button event (a hold while a gauge fills: the game's own event on the
// game's own data) runs in that player's identity, on that player's character.
//
// The game keeps one push-button manager. Player 1's press of a lever target starts its event (Init: the row named by the
// target's ObjectLayout row, the pusher = the party leader), the field step ticks the event's steps each frame (the stand
// point, a slide to it, the hold read from the FieldAction key, the animation phases, the press of the target, the end),
// the finished-animation notify clears the phases' arrival flags, and the field step frees the object when the event is
// done. Every row is authored for a named character, so for a seat's character the event runs with:
//   - the row's named character exchanged for the player's own character's name for the length of Init (the game's own
//     ctor then builds one leader entry, as for player 1), and the row's animations left out when the row does not name
//     that character (the game's own path for a character with nothing to play: a stand at the lever, the gauge, the
//     lever moving; world/push_event_rules.h decides which rows a player may work);
//   - Init, the step and the free run inside the player's Leader, Primary, Operator and UiSlot scopes with the player's own
//     pad (the hold is read from the player's pad into the event's held byte, as the game reads player 1's) and the player's
//     own target icons (the gauge shows on the player's icon);
//   - the finished notify, which the game may send from the animation side, kept for the player's next step and run there
//     inside the same scopes (one frame late at most) under the game's own lock; one that comes inside that step runs at once.
// One event at a time (the game's own admit test: its active list empty). A watchdog ends an event stuck in a wait (the
// level load, a finish that never came) by setting its end step: a stall on everyone must be impossible. A player leaving
// mid-event ends it the same way before their scopes go. The event's field lock hands the game's leader to its AI as player
// 1's event does; the session keeps the pressing player's own character with its player (players/field_hold.h).
// Operation boards (a quick press) run as the player's own field action (world/traversal_action.h); only their end
// trigger needs the player's leader for the level to hear it, which that module sends.
// Logged: Info at install and placement, per event start and end; Debug per step change; the `levers` reading.

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::push_events {

/// Checks the research's sites and functions and redirects the four calls (each inside one block, placed here). False
/// (logged) if a check fails: players 2 and 3 then press no levers.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread, each engine tick: the seat's local player and member while its player is in (null otherwise).
void TickSeat(int seat, void* localPlayer, void* member) noexcept;
/// Game thread, each engine tick after the seats.
void Tick() noexcept;

/// The target `name` (an ObjectLayout row) names a push-button row that `member`'s player may work (push_event_rules).
bool LeverRow(std::uint64_t name, const void* member) noexcept;
/// A press of such a target by `seat` may start an event now: the calls placed, no event running, the member not in battle
/// and running no field action.
bool Admitted(int seat) noexcept;
/// `seat`'s own event runs on the target `name` now: its icon stays the player's to hold (the game's pick keeps it in
/// its hold state with the gauge; marked disabled, the pick would set it to none and the hold step would end the event).
bool Running(int seat, std::uint64_t name) noexcept;

/// The seat's player is leaving (its scopes still bound): its running event, if any, is ended.
void OnSeatLeaving(int seat) noexcept;

/// The `levers` reading.
std::wstring Reading() noexcept;

}  // namespace lss::world::push_events
