#pragma once

// Player 2's lock camera at the edges outside the operator's tick (RE-lock-camera-ledger.md).
//
// Player 2's operator ticks in the four scopes (the primary, the operator, the leader, the target-field copy), so
// every read inside its tick is player 2's. This module covers the game's lock writers that run outside both
// players' contexts, and two readings:
//   - an enemy's transform (the AI controller's TransformCharacter exec) moves the displayed target to the new form,
//     and a script locks the leader on the nearest enemy (the battle API's SetDisableTarget exec): each runs the
//     game's own call a second time inside player 2's scopes (the primary, the operator, the leader and the copy),
//     so player 2's lock follows the transform too and a scripted lock takes both players (row X9);
//   - `lock-camera-two.requests`: the battle camera requests that re-aimed each operator's lock camera;
//   - `lock-camera-two.restore`: which context runs the command menu's target-list end (it restores the lock and
//     asks the Command camera flavor) for player 2: its controller's tick, where the copy is in, or a pad-2 key
//     event, where it is not yet (row X5, read before a lever).
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::lock_camera_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick: places the call redirect that crosses a block, from the game thread.
void Tick() noexcept;

}  // namespace lss::battle::lock_camera_two
