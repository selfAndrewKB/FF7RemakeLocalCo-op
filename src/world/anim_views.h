#pragma once

// Characters near any player animate as the game animates them near player 1 (research/RE-anim-two-views.md).
//
// Square Enix's animation budget judges every skeletal mesh once per frame from ONE camera snapshot, player 1's: a
// character outside player 1's 60-degree cone and more than 5 m away gets reduced cloth and hair, beyond 20 m it stops
// refreshing its bones, and its update rate and budget priority drop. So the world near a further player went stiff
// or froze whenever player 1 looked away. The game has no input for a second camera (one snapshot, thresholds written
// into the code, no list of extra measurements), so the one recreated piece is the input: which camera
// the game's judge reads for each mesh (documented in the plan and the research).
//
// Two vtable slots of the judge are replaced. For each mesh the wrapper measures the owner's distance and angle to
// player 1's camera and to each further player's with the judge's own thresholds (world/anim_choice.h: the best of
// them, a tie keeping player 1's, then the lower seat's); when a further player's camera sees it better, the
// allocator's 32-byte snapshot holds that player's for the length of that one call and is put back after. The game's
// judge then runs exactly once per mesh per frame, as always, and every decision and write in it stays the game's.
// Each further player's snapshot is made once per frame in the game's own form (the 0.1 s prediction) by a wrapper on
// the allocator's Update slot, from that player's camera manager at the moment the game reads player 1's. Serial,
// game thread, the world's pre-actor tick; nothing runs on other threads.
//
// Logged: install per slot (Info); every 10 s the meshes judged, the share judged from each further player's camera
// and the judging time, and each further player's character's camera, distance, angle and cloth mode (Debug); the
// switch (Info).

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::anim_views {

/// Replaces the three slots (each checked; logged). False if one differs (nothing is replaced then).
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick (before the world tick), per built seat: the seat's camera manager while its player
/// is in (null otherwise), and the seat's character for the log.
void TickSeat(int seat, void* seatCameraManager, void* seatMember) noexcept;

/// Game thread, each engine tick after the seats': the 10 s summary.
void Tick() noexcept;

/// The comparison switch (`anim on|off`, on at start).
void SetEnabled(bool on) noexcept;

/// The `world.anim` reading: the last 10 s summary.
std::wstring Reading() noexcept;

}  // namespace lss::world::anim_views
