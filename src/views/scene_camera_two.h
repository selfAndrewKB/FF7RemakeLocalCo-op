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
// Player 2's manager, fed by player 2's own field camera operator, keeps its slot 24 through a scene.
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
// Readings `scene.*`, `camera-slots.*`, `scene-camera-two.state`; the switch `scene-camera-two on|off` (on). Game thread.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::views::scene_camera_two {

/// Rebases the globals, finds the game's slot setters, the reset and the cutscene manager's playing test, redirects
/// the cutscene end's reset call, and registers the readings and the switch. False (logged) if a check fails: the
/// readings may still work, the mirror is stood aside.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once per engine tick with each local player's camera manager (null when absent): reads and logs what changed.
void Tick(void* managerOne, void* managerTwo) noexcept;

/// Right before a camera manager's UpdateCamera. Player 1's (`managerTwo` null): its cut bit is noted for this frame.
/// Player 2's: the scene slots are mirrored onto it; the caller holds player 2's primary scope (the game's
/// primary-scoped setters then reach player 2's manager).
void BeforeUpdateCamera(void* managerOne, void* managerTwo) noexcept;

}  // namespace lss::views::scene_camera_two
