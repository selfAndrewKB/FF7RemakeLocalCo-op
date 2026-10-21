#pragma once

// Party members never vanish near a camera while player 2 is in (the user's reminder of 2026-10-21: "disable the
// camera making party members disappear when they are too close to it"; research/RE-camera-fades.md L1 and L2).
//
// What hid them is Square Enix's render near-fade on each mesh. Every view fades a mesh its own camera is within a
// few centimetres of, but the fade state is kept once per mesh and reset when a second view evaluates it in the same
// frame. So player 2's view snaps it every frame: a member next to player 2's camera vanished in both halves at once,
// and one next to player 1's camera never faded. Two views cannot draw this fade correctly.
//
// The game itself switches the fade off for the character the player controls: at every leader change it lists the
// reason "Default" on the new leader (EndCharacter_ApplyLeaderCameraFade), and while any reason is listed the mesh
// does not near-fade. While player 2 is in, the mod lists a reason of its own on every party member through the same
// call, so each one stays visible as the player's own character does in single player. When player 2 leaves, it
// takes its reason off again, and a member with no reason left fades as before. Only the mod's own name is ever added
// or removed: the game's reasons (the leader's, cutscenes', battle cameras') are untouched. A member whose fade is
// found on again while the reason is listed (a scripted force, a new actor) is switched off again at the next check.
// Nothing is called into the game at uninstall (the reason then simply stays). Game thread only.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::party_fade {

/// Finds the game's switch, its name constructor and the party lookup; logged either way. False if a check failed
/// (party members then fade as the game decides).
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick: whether player 2 is in. The party is checked on every change and every few ticks.
void Tick(bool playerTwoIn) noexcept;

/// The `world.fade` reading: each party member's fade switch and whether the mod's reason is listed on it.
std::wstring Reading() noexcept;

}  // namespace lss::world::party_fade
