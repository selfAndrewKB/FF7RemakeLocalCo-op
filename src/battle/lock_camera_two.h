#pragma once

// Each seat's lock camera at the edges outside the operator's tick (RE-lock-camera-ledger.md; the seats follow
// plan/seats.md step 3).
//
// A seat's operator ticks in the four scopes (the primary, the operator, the leader, the seat's target-field copy),
// so every read inside its tick is the seat's. This module covers the game's lock writers that run outside every
// player's context, and three readings:
//   - an enemy's transform (the AI controller's TransformCharacter exec) moves the displayed target to the new form,
//     and a script locks the leader on the nearest enemy (the battle API's SetDisableTarget exec): each runs the
//     game's own call again for every seat whose player holds a member, inside the seat's scopes (the primary, the
//     operator, the leader, and the seat's target-field copy, which must be in), so each seat's lock follows the
//     transform too and a scripted lock takes every player (row X9);
//   - `lock-camera-two.requests`: the battle camera requests that re-aimed each operator's lock camera, per seat and
//     for any other operator;
//   - `lock-camera-two.restore`: which context runs the enemy detail window's close delegate (it restores the lock
//     and asks the Command camera flavor; pass S-RMK-CMDCAM found it is the window's, not a command-menu state's,
//     so it counts only when that window closes): player 1's controller tick, a seat's, where its copy is in, or a
//     seat's pad's key event, where it is not yet (row X5, read before a lever); and the re-targets repeated per
//     seat;
//   - `lock-camera-two.frames`: each seat's operator tick, read around it (the ledger's live checks L1, L2, L5): the
//     control yaw it starts from against the yaw it last wrote back (another writer in between), the frame stamp
//     that would skip its write-back, the character under another controller, the yaw it moves per tick and its
//     reversals (a stutter), and the seat's pad's rotation input while the seat is locked. A Debug line names a burst
//     of reversals.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::lock_camera_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick: places the call redirect that crosses a block, from the game thread.
void Tick() noexcept;

/// Around the seat's operator tick, and after the seat's input step: the `lock-camera-two.frames` readings.
void BeforeOperatorTick(int seat, void* seatOperator, void* seatController) noexcept;
void AfterOperatorTick(int seat, void* seatOperator) noexcept;
void AfterInput(int seat, void* seatController) noexcept;

/// The seat's player leaves: its frame readings start over for the next player.
void OnLeave(int seat) noexcept;

}  // namespace lss::battle::lock_camera_two
