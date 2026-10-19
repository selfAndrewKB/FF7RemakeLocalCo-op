#pragma once

#include <cstdint>

// Player 2's own field camera: a second AEndFieldCameraOperator, driven by the game's own code.
//
// The game has one field camera operator (a world actor, g_EndGameRoot+0x9B0) that follows the party leader
// and writes the primary camera manager's pose slot 24 each tick; PartyManager+0 names the primary controller.
// Player 2's camera is the same operator class, spawned with the game's own spawn call and parameters, and run
// with two globals swapped for the length of each of its calls (Remake/Documentation/research/RE-camera.md
// §9.6, RE-impl-facts-v0.2.md):
//   - PartyManager+0 = player 2's controller, so the operator reads player 2's control rotation and writes
//     player 2's camera manager;
//   - the root's operator weak pair = player 2's operator, so the game's own "the operator" lookups inside
//     those calls find player 2's.
// The same two swaps hold around player 2's ProcessPlayerInput, where the right stick reaches "the operator".
// Each player's stick calls see only that player's pad: the input object merges every pad into one value
// per frame, so for the length of a controller's input and movement calls the merged value is replaced by
// that player's pad record, and put back after (pad 1 = XInput user 0, pad 2 = user 1).
// All of this runs on the game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::players::player_two_camera {

/// A UE4 weak pointer pair {GUObjectArray index, serial number} for a live object, made the way the game makes one
/// (the serial allocated if the object has none yet). False if the object cannot be read. Game thread.
bool WeakPairOf(void* object, std::int32_t (&pair)[2]) noexcept;

/// The object a weak pair names, if it is still that object (same serial) and reachable; else nullptr.
void* ObjectOf(const std::int32_t (&pair)[2]) noexcept;

/// Checks every function, slot and global, and replaces the operator's BeginPlay and Tick slots and the
/// End controller's ProcessPlayerInput and Tick_FieldMove slots. False (logged) if anything differs.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, after player 2's controller exists: spawns player 2's operator and points it at the
/// player-2 member (Aerith in the test party). False (logged) if the field has no operator or the member
/// is not loaded.
bool Start(void* playerOneController, void* playerTwoController) noexcept;

/// Game thread, before player 2's controller is removed: forgets player 2's operator and returns it (or
/// null if it is gone) for the caller to destroy.
void* Take() noexcept;

/// Game thread, every engine tick: keeps the game's world-tick phase hook wrapped, so that player 2's
/// controller gets the game's input step right after player 1's (the game ticks only the primary controller's
/// input; RE-input-p2.md §1). The game sets that hook pointer at runtime, hence the per-tick check.
void MaintainInputTick() noexcept;
/// Puts the game's phase hook back (unload).
void RemoveInputTick() noexcept;

/// Test input (control scripts): while held, `user`'s stick record reads these values (LX, LY, RX, RY,
/// -1..1) inside that player's stick scopes, in place of the pad. Only acts while player 2 is in. Game thread.
void HoldStick(unsigned user, const float (&axes)[4]) noexcept;
void ReleaseStick(unsigned user) noexcept;

/// Logs how often each player's input, movement and camera calls ran since the last report. Game thread.
void LogCounts() noexcept;

}  // namespace lss::players::player_two_camera
