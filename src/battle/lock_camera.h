#pragma once

// Each seat's lock camera at the edges outside the operator's tick (research/RE-lock-camera-ledger.md).
//
// A seat's operator ticks in the four scopes (the primary, the operator, the leader, the seat's target-field copy),
// so every read inside its tick is the seat's. This module covers the game's lock writers that run outside every
// player's context, and three readings:
//   - an enemy's transform (the AI controller's TransformCharacter exec) moves the displayed target to the new form,
//     and a script makes a character targetable or not (the battle API's SetDisableTarget exec), which moves a
//     displayed target off a character disabled under it: each runs the game's own call again for every seat whose
//     player holds a member, inside the seat's targeting scopes (battle/targeting.h kTargetingScopes: its own command menu
//     too, so the game's guard never hands it a target picked in player 1's menu) and its target-field copy, so each
//     seat's target follows the transform and the disable too;
//   - `lock-camera.requests`: the battle camera requests that re-aimed each operator's lock camera, per seat and
//     for any other operator;
//   - `lock-camera.restore`: which context runs the enemy detail window's close delegate (it restores the lock
//     and asks the Command camera flavor; the delegate is the window's, not a command-menu state's,
//     so it counts only when that window closes): player 1's controller tick, a seat's, where its copy is in, or a
//     seat's pad's key event, where it is not yet; and the re-targets repeated per
//     seat;
//   - `lock-camera.frames`: each seat's operator tick, read around it: the
//     control yaw it starts from against the yaw it last wrote back (another writer in between), the frame stamp
//     that would skip its write-back, the character under another controller, the yaw it moves per tick and its
//     reversals (a stutter), and the seat's pad's rotation input while the seat is locked. A Debug line names a burst
//     of reversals;
//   - `lock-camera.jumps`: every player's camera after its operator's tick (player 1's while a further player is
//     in): a tick that turned or moved it far faster than a stick, a follow or a dodge does (a cut, a reset, another
//     writer) opens a burst of jumps, a Debug line "camera jump" with what surrounded it: the player's lock and
//     displayed target and when they last changed, a battle camera request in that tick, the camera behaviours
//     (flavors) on, and the other players' locks, targets and jumps; a quiet spell closes the burst (a second line
//     when it held more than one jump). A reading only; nothing is changed.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::lock_camera {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick: places the call redirect that crosses a block, from the game thread.
void Tick() noexcept;

/// Around the seat's operator tick, and after the seat's input step: the `lock-camera.frames` readings, and the
/// jump watch (`ownTargets`: the seat's target-field copy is in, so the battle system's fields are the seat's).
void BeforeOperatorTick(int seat, void* seatOperator, void* seatController) noexcept;
void AfterOperatorTick(int seat, void* seatOperator, float deltaSeconds, bool ownTargets) noexcept;
/// Around player 1's operator's tick while a seat is in: the jump watch on player 1's camera (a battle camera request
/// found between the two is player 1's).
void BeforePlayerOneOperatorTick() noexcept;
void AfterPlayerOneOperatorTick(void* cameraOperator, float deltaSeconds) noexcept;
void AfterInput(int seat, void* seatController) noexcept;

/// The seat's player leaves: its frame readings and its camera's jump watch start over for the next player (an open
/// burst of jumps, the seat's and player 1's, ends with its line).
void OnLeave(int seat) noexcept;

}  // namespace lss::battle::lock_camera
