#pragma once

// No character turns see-through in a ladder, squeeze, crawl or similar move while a further player is in
// (research/RE-overlap-fade-two.md).
//
// The game fades a character in a field action that is not the party leader's to 0.3 (the vent crawl's to nothing)
// while a party member before it in formation order is close, so an AI companion right behind the player's character
// does not clip through it. Each action state does it in its own vtable +0x58, which the state's post-tick calls on the
// game thread outside every scope of the mod, where the party leader is always player 1's. So in co-op a further
// player's character fades near player 1's (in every view: the fade is per actor), and the AI near either.
//
// While a further player is in, the mod's function in those eight slots does not call the game's when the fade is off
// for that state (its "not faded" byte set), so a fade never starts. A fade that is already on (from before a join) is
// left to the game's function, which can then only fade the character back in, the first tick nobody is close; the
// action's own exit does it at the latest. The leader's own branch (the vent crawl's) always runs as the game's. With
// no further player in, every call goes to the game's function. The mod writes no game state. Switch `overlap-fade`
// (on: the fade kept off while a further player is in; off: the game's fade as in single player).
// Logged: Info at install and when the fade is kept off or allowed again; Debug per state when what the mod does with
// it changes; the `world.overlap-fade` reading.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::overlap_fade {

/// Checks the eight slots and the five functions they hold, and replaces each slot that matches (logged; a slot that
/// does not match keeps the game's fade for that move).
bool Install(const platform::GameImage& image) noexcept;

/// Puts the game's functions back.
void Remove() noexcept;

/// Game thread, each engine tick: how many further players are in.
void Tick(int seatsIn) noexcept;

/// The `world.overlap-fade` reading.
std::wstring Reading() noexcept;

}  // namespace lss::world::overlap_fade
