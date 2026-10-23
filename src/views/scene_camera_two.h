#pragma once

// The scene camera on player 2's view: a cutscene, a talk scene or an event camera plays on player 2's view as it
// does on player 1's (research/RE-cutscene-camera-two.md, pass S-RMK-CUTCAM; RE-cutscene-scene-state.md; RE-camera.md
// §2.2-2.4). Rebirth collapsed to player 1's single view for scenes (its whole_screen module); here the scene camera
// is given to player 2's camera manager instead, the user's call of 2026-10-21.
//
// Each camera manager shows the lowest active of its 25 pose slots and blends between its own slots. A scene writes
// the primary manager's slots only (every writer resolves the primary controller's manager): the cutscene pose into
// slot 8 each frame, a talk camera into 14, an event camera into 18, the start and end blends as on-switches with a
// 60-byte record in slots 2-7, 9-13, 16, 17 (seeded with the source pose: the field camera's slot 24 for 6, 12, 16).
// Player 2's manager, fed by player 2's own field camera operator, keeps its slot 24 through a scene. Every further
// player gets the same, each seat with its own copy record (plan/seats.md step 6: player 3).
//
// The lever, the game's own setters on player 2's manager, applied right before its UpdateCamera each frame while a
// scene holds the players (the cutscene manager playing, or the game's field control lock):
//   - the pose slots (8, 14, 18 and the other non-blend slots) are copied from player 1's manager every frame, the
//     active byte included (the same pose for every view: the writers read no player);
//   - a blend slot is set once, when it turns on for player 1: its record, and its seed pose; a blend from the
//     field camera is seeded with player 2's own slot 24, so the blend starts from player 2's camera; the manager's
//     own UpdateCamera then runs the blend with its own length and ends it (the end blend lands on player 2's own
//     field camera for free: the "to field" target is always slot 24 of the manager that blends);
//   - a camera cut (player 1's manager's cut bit, set by the cut instance on the first controller's manager only) is
//     raised on player 2's manager through the game's primary-scoped cut, so the anti-aliasing history resets there too;
//   - the cutscene's end reset (EV/MV: every slot off, 24 on, the view target the own main camera) runs on player
//     2's manager as well, by a redirect of the one call the cut instance makes;
//   - when the scene releases the players, the copied pose slots are switched off on player 2's manager, and a blend
//     still running there finishes by itself.
// Not covered (documented): the field operator's own return (flavor 6 seeded with the cut's end pose, the re-seat
// behind the leader) runs for player 1's operator only; player 2's camera returns through the end blend to wherever
// its operator stands. Sequencer camera shakes, rumble: player 1's.
// A mirror player's manager (views/mirror_player, v0.42.0; up to two mirrors since the seats plan's step 5) takes the
// same copy for EVERY slot, the field camera's 24 included, every frame, so its view is player 1's: a blend seeded as
// player 1's was (player 1's slot at the switch-on), the cut raised, the end reset run on it too. One source for
// every copy; a mirror needs no scene to run. Each mirror has its own copy record, by the mirror module's slot.
// Readings `scene.*`, `camera-slots.*`, `scene-camera-two.state`; the switch `scene-camera-two on|off` (on). Game thread.

#include "players/seats.h"

#include <array>
#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::views::scene_camera_two {

/// Rebases the globals, finds the game's slot setters, the reset and the cutscene manager's playing test, redirects
/// the cutscene end's reset call, and registers the readings and the switch. False (logged) if a check fails: the
/// readings may still work, the mirror is stood aside.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each seat's camera manager while its player is in (null when out), by seat; [0] unused.
using SeatManagers = std::array<void*, players::scopes::kMaxPlayers>;

/// Once per engine tick with player 1's camera manager and each seat's: reads and logs what changed; a seat that
/// left has its copy forgotten.
void Tick(void* managerOne, const SeatManagers& seatManagers) noexcept;

/// Right before a camera manager's UpdateCamera. Player 1's (`seat` 0): its cut bit is noted for this frame. A
/// seat's (`manager` its own): the scene slots are mirrored onto it; the caller holds the seat's primary scope (the
/// game's primary-scoped setters then reach the seat's manager).
void BeforeUpdateCamera(void* managerOne, int seat, void* manager) noexcept;

/// The mirror players' copies (views/mirror_player), one per mirror slot 0..kMirrorSlots-1.
inline constexpr int kMirrorSlots = 2;
/// Mirror `slot`'s camera manager: every slot of player 1's manager `managerOne` is set on `manager` right before its
/// update, so its view is player 1's. All null: no mirror in that slot.
void SetMirror(int slot, void* controller, void* manager, void* managerOne) noexcept;
/// The mirror slot whose camera manager this is, or -1.
int MirrorSlotOf(const void* manager) noexcept;
void* MirrorController(int slot) noexcept;
/// Player 1's manager as the mirrors' source (null without a mirror): its update notes the cut for the mirrors.
void* MirrorSource() noexcept;
/// Right before mirror `slot`'s manager's UpdateCamera; the caller holds that mirror's primary scope.
void BeforeMirrorUpdate(int slot) noexcept;
/// The mirror copies' state and counts, for the `mirror-player.state` reading.
std::wstring MirrorPlayerText();

}  // namespace lss::views::scene_camera_two
