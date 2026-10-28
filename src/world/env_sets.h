#pragma once

// Each further player's view hides what the area's design hides under that player, as player 1's view does under
// player 1 (research/RE-picture-variants.md §5, §9, §10; the orphanage picture's flicker: three stacked picture
// variants, two of them hidden only while player 1 stands in the orphanage's set; the user, 2026-10-21, against
// hiding world-wide: "possibly hiding things from P1's view that P2 can't see? You sure we don't have a cleaner
// implementation?"; the seats follow plan/seats.md step 3).
//
// Square Enix's EnvironmentSet manager traces straight down from the party leader every frame; the material of the
// ground hit names the area's set, and the field's EnvironmentSetWrapper row lists the actors that set hides (hidden
// reason 13, world-wide, collision off too). A further player's character is never asked. The world stays the
// game's: player 1's set, applied by the game alone, unchanged. For each further player the mod drives two of the
// game's calls once more and uses the engine's per-player hidden list:
//   1. the trace: after the game's own from player 1's character, the game's trace function again from the player's
//      character, into the mod's own handle for that seat;
//   2. the read: after the game's own read of player 1's set, the game's read of the seat's handle into the mod's
//      own block for that seat. The read writes the area's name into the save data (the name a no-hit falls back
//      to); the mod blanks it for each seat's read and puts it back exactly, so player 1's fallback and the saves
//      stay player 1's. When the ground under a further player names no set, that player's last set is kept, as
//      the game keeps player 1's. A new set is taken at once, then no other for 1 s (the game's own pause after a
//      change of player 1's set; the one recreated rule);
//   3. the view: the actors the seat's set hides, found the way the game's apply finds them (each level of the set
//      that is in the manager's lookup set, the character registry's name map), with every actor attached below
//      them, go into that seat's controller's HiddenActors. Stock UE 4.18 builds the view from that list alone
//      (ULocalPlayer::CalcSceneView): those meshes leave that player's view and stay in the others'. The mod adds
//      and removes only its own entries; the engine grows the list (its own resize).
// Not per view (§10.4): collision, lights, decals and point/spot shadows follow the world, which is player 1's set;
// an actor a further player's set shows but player 1's set hides stays hidden for everyone (counted in the reading).
// Hold mode (one scripted set for everyone): nothing hidden for a further player alone.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::env_sets {

/// Checks the calls and the game functions against the research and places the read redirect (logged). The trace's
/// call crosses a block: Tick places it. False if anything differs (every further player's view then shows player
/// 1's world).
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick (outside the world tick), per built seat: the seat's character and controller while
/// its player is in (null otherwise).
void TickSeat(int seat, void* seatMember, void* seatController) noexcept;

/// Game thread, each engine tick after the seats': places the trace redirect on the first call.
void Tick() noexcept;

/// The switch (`env-sets on|off`, on at start): off empties the mod's entries in every further player's list at the
/// next read.
void SetEnabled(bool on) noexcept;

/// A further player's last ground set (its three ids: area, environment set, planner level set) as text; game thread.
std::wstring SeatSetText(int seat) noexcept;

/// The `world.env-sets` reading: each further player's set, the actors hidden in their view, what could not be.
std::wstring Reading() noexcept;

}  // namespace lss::world::env_sets
