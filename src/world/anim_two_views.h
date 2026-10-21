#pragma once

// Characters near player 2 animate as the game animates them near player 1 (research/RE-anim-two-views.md; Rebirth's
// player_two/anim_two_views, cleanup-2026-10).
//
// Square Enix's animation budget judges every skeletal mesh once per frame from ONE camera snapshot, player 1's: a
// character outside player 1's 60-degree cone and more than 5 m away gets reduced cloth and hair, beyond 20 m it stops
// refreshing its bones, and its update rate and budget priority drop. So the world near player 2 went stiff or froze
// whenever player 1 looked away. The game has no input for a second camera (one snapshot, thresholds written into the
// code, no list of extra measurements as Rebirth has), so the one recreated piece is the input: which camera the
// game's judge reads for each mesh (documented in the plan's §2 and the research).
//
// Two vtable slots of the judge are replaced. For each mesh the wrapper measures the owner's distance and angle to
// both cameras with the judge's own thresholds (world/anim_choice.h); when player 2's camera sees it better, the
// allocator's 32-byte snapshot holds player 2's for the length of that one call and is put back after. The game's
// judge then runs exactly once per mesh per frame, as always, and every decision and write in it stays the game's.
// Player 2's snapshot is made once per frame in the game's own form (the 0.1 s prediction) by a wrapper on the
// allocator's Update slot, from player 2's camera manager at the moment the game reads player 1's. Serial, game
// thread, the world's pre-actor tick; nothing runs on other threads.
//
// Logged: install per slot (Info); every 10 s the meshes judged, the share judged from player 2's camera and the
// judging time, and player 2's character's camera, distance, angle and cloth mode (Debug); the switch (Info).

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::anim_two_views {

/// Replaces the three slots (each checked; logged). False if one differs (nothing is replaced then).
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick (before the world tick): player 2's camera manager while the split is shown (null
/// otherwise), and player 2's character for the log.
void Tick(void* cameraManagerTwo, void* memberTwo) noexcept;

/// The comparison switch (`anim-two on|off`, on at start).
void SetEnabled(bool on) noexcept;

/// The `world.anim` reading: the last 10 s summary.
std::wstring Reading() noexcept;

}  // namespace lss::world::anim_two_views
