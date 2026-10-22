#pragma once

// Player 2's lock camera at the edges outside the operator's tick (RE-lock-camera-ledger.md).
//
// Player 2's operator ticks in the four scopes (the primary, the operator, the leader, the target-field copy), so
// every read inside its tick is player 2's. This module covers the game's lock writers that run outside both
// players' contexts, and three readings:
//   - an enemy's transform (the AI controller's TransformCharacter exec) moves the displayed target to the new form,
//     and a script locks the leader on the nearest enemy (the battle API's SetDisableTarget exec): each runs the
//     game's own call a second time inside player 2's scopes (the primary, the operator, the leader and the copy),
//     so player 2's lock follows the transform too and a scripted lock takes both players (row X9);
//   - `lock-camera-two.requests`: the battle camera requests that re-aimed each operator's lock camera;
//   - `lock-camera-two.restore`: which context runs the enemy detail window's close delegate (it restores the lock
//     and asks the Command camera flavor; pass S-RMK-CMDCAM found it is the window's, not a command-menu state's,
//     so it counts only when that window closes) for player 2: its controller's tick, where the copy is in, or a
//     pad-2 key event, where it is not yet (row X5, read before a lever);
//   - `lock-camera-two.frames`: player 2's operator tick, read around it (the ledger's live checks L1, L2, L5): the
//     control yaw it starts from against the yaw it last wrote back (another writer in between), the frame stamp
//     that would skip its write-back, the member's controller, the yaw it moves per tick and its reversals (a
//     stutter), and pad 2's rotation input while player 2 is locked. A Debug line names a burst of reversals.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::lock_camera_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick: places the call redirect that crosses a block, from the game thread.
void Tick() noexcept;

/// Around player 2's operator tick, and after player 2's input step: the `lock-camera-two.frames` readings.
void BeforeOperatorTick(void* operatorTwo, void* controllerTwo) noexcept;
void AfterOperatorTick(void* operatorTwo) noexcept;
void AfterInput(void* controllerTwo) noexcept;

}  // namespace lss::battle::lock_camera_two
