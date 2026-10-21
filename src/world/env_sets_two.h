#pragma once

// The ground under player 2 hides what the area's design hides, as the ground under player 1 does
// (research/RE-picture-variants.md §5, §9; the orphanage picture's flicker: three stacked picture variants, two of them
// hidden only while player 1 stands in the orphanage's set).
//
// Square Enix's EnvironmentSet manager traces straight down from the party leader every frame; the material of the
// ground hit names the area's set, and the field's EnvironmentSetWrapper row lists the actors that set hides (hidden
// reason 13, world-wide). Player 2's character is never asked. The mod drives the same three game calls once more:
//   1. the trace: after the game's own from player 1's character, the game's trace function again from player 2's
//      character, into the mod's own handle;
//   2. the read: after the game's own read of player 1's set, the game's read of player 2's handle into the mod's own
//      block; player 2's entries for actors player 1's list does not name are added after player 1's with the game's
//      own append (player 1 first). The read writes the area's name into the save data (the name a no-hit falls back
//      to); the mod blanks it for player 2's read and puts it back exactly, so player 1's fallback and the saves stay
//      player 1's. When player 2's ground names no set, player 2's last set is kept, as the game keeps player 1's;
//   3. the apply: it finds actors only in the levels it is handed, so it is handed player 1's levels plus the levels
//      of player 2's set (a copy; the game's own list, which also drives sublevel streaming, is untouched).
// The game re-applies only when player 1's set changes; when the entries the mod adds change (player 2 entered or left
// a set, or left the game), the mod clears the manager's remembered set id so the game's own change path runs (its own
// apply, which also puts back the actors a list no longer names). Never in hold mode, never when player 1's read built
// no set. Sublevel streaming stays player 1's [ASSUMPTION: player 2's half can lose sublevels when player 1 is in
// another area; not handled].

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::env_sets_two {

/// Checks the three calls and the game functions against the research and places the read and apply redirects
/// (logged). The trace's call crosses a block: Tick places it. False if anything differs (the game alone then).
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick (outside the world tick): player 2's character while player 2 is in (null
/// otherwise). Places the trace redirect on the first call.
void Tick(void* memberTwo) noexcept;

/// The switch (`env-sets on|off`, off at start since v0.32.0: the game hides world-wide, so player 1 would lose what
/// player 2's ground hides): off drops player 2's entries at the game's next read.
void SetEnabled(bool on) noexcept;

/// The `world.env-sets` reading: player 2's set, the actors it adds, the game's re-applies.
std::wstring Reading() noexcept;

}  // namespace lss::world::env_sets_two
