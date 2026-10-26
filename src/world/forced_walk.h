#pragma once

// Every player walks where the story makes player 1 walk (the user's report of 2026-10-25: in a forced walk, player 1
// and the AI walk but a further player runs; research/RE-forced-walk-two.md; Rebirth's move_type_two was read: its
// limits sit on the controller and are set through a setter, so its lever, the setter in the seat's scope, does not
// carry).
//
// Remake keeps a character's walk limits on the character itself (a move type and a dash type per layer), and each
// player's controller reads only its own character's. The story's acts store the limit straight into the party leader's
// character, with no setter: a scripted walk (layer 1) and a guided walk paced by the leader's distance to a point
// (layer 4) every frame while they run, the walk after a scene (layer 2) at its begin and its switch to a jog; each
// clears its layer at its end. So only player 1's character was ever capped.
//
// After each Begin or Tick of those acts, if the character the act wrote is player 1's, the mod writes the same layer's
// pair onto every further player's member: the game's value at the game's moment. After each End, the layer's clear
// goes onto every further player's member whichever character the game cleared (player 1 may have switched members
// since). A member taken by a player gets player 1's current layers at once (a join in the middle of a walk), and a
// member given back (a leave) has the three layers cleared, unless it is player 1's own now or another seat's. The
// indoor rooms' limit (layer 6) is not copied: each room writes its own player's (players/nav_area_two.h), and a copy
// would have nothing to clear it. Nothing is followed while a seat's Leader scope is open (never expected around an act). Switch
// `forced-walk` (on: further players follow player 1's story walk limits; off: their copies cleared, they run free).
// At unload the slots go back and the members keep the limits last copied (the next take after a reload repairs them).
// Logged: Info at install and at the switch; Debug when a member's limit changes and when an act wrote another
// character than player 1's; Trace per followed call; the `world.forced-walk` reading.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::forced_walk {

/// Checks the acts' slots and functions and the party lookups, and replaces the slots (logged; false unless all seven
/// were placed, and then none is kept: further players run free in a story walk).
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, each engine tick: the seat's member while its player is in (null otherwise).
void TickSeat(int seat, void* member) noexcept;

/// The `world.forced-walk` reading.
std::wstring Reading() noexcept;

}  // namespace lss::world::forced_walk
