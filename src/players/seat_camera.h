#pragma once

#include <cstdint>

// Each seat's own field camera: a further AEndFieldCameraOperator per seat, driven by the game's own code.
//
// The game has one field camera operator (a world actor, g_EndGameRoot+0x9B0) that follows the party leader
// and writes the primary camera manager's pose slot 24 each tick; PartyManager+0 names the primary controller.
// A seat's camera is the same operator class, spawned with the game's own spawn call and parameters, and run
// with two globals swapped for the length of each of its calls (research/RE-camera.md,
// RE-impl-facts-v0.2.md):
//   - PartyManager+0 = the seat's controller, so the operator reads that controller's control rotation and
//     writes that player's camera manager;
//   - the root's operator weak pair = the seat's operator, so the game's own "the operator" lookups inside
//     those calls find it.
// The same two swaps hold around the seat's ProcessPlayerInput, where the right stick reaches "the operator".
// Each player's stick calls see only that player's pad: the input object merges every pad into one value
// per frame, so for the length of a controller's input and movement calls the merged value is replaced by
// that player's pad record, and put back after (pad 1 = XInput user 0, a seat's pad = user `seat`).
// The slot replacements find the seat from the object they are handed (its controller, operator or camera
// manager), so one set of replacements serves every seat. All of this runs on the game thread.
// Seats: players/seats.h (the seats built, each a further player). State owned here: the stick holds and the call counts; each
// seat's controller and operator are the registry's (players/seat_objects.h: the session binds the controller at the
// join, Start binds the operator it spawns, Take unbinds it). The battle modules this one calls around a tick (the
// lock camera, the sequence cameras, the targeting copy) and the operator's satellites (nav areas, battle mode, the
// override route) are per seat.

namespace lss::platform {
class GameImage;
}

namespace lss::players::seat_camera {

/// Checks every function, slot and global, and replaces the operator's BeginPlay and Tick slots, the End
/// controller's ProcessPlayerInput and Tick_FieldMove slots and the End camera manager's UpdateCamera slot. False
/// (logged) if anything differs.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, once the seat's controller holds `member` and the session has bound that controller in the
/// registry (players/seat_objects.h): spawns the seat's operator and points it at the member. False (logged) if the
/// field has no operator or the spawn failed.
bool Start(int seat, void* playerOneController, void* seatController, void* member) noexcept;

/// Game thread, before the seat's controller is removed: forgets the seat's operator and returns it (or
/// null if it is gone) for the caller to destroy.
void* Take(int seat) noexcept;

/// Puts the game's input call back (unload). Install redirects it so that each seat's controller gets the game's
/// input step right after player 1's, at the same point of the frame (the game ticks only the primary controller's
/// input; RE-input-p2.md, RE-camera-jump-combo.md). One aligned block, so any thread may restore it.
void RemoveInputTick() noexcept;

/// Test input (control scripts): while held, `user`'s stick record reads these values (LX, LY, RX, RY,
/// -1..1) inside that player's stick scopes, in place of the pad. Only acts while that seat is in. Game thread.
void HoldStick(unsigned user, const float (&axes)[4]) noexcept;
void ReleaseStick(unsigned user) noexcept;

/// Logs how often each player's input, movement and camera calls ran since the last report. Game thread.
void LogCounts() noexcept;

/// The seat whose own operator's Tick runs now (inside its scopes), else 0.
int TickingSeat() noexcept;

/// Whether the caller is the game thread (as noted each engine tick; false before the first).
bool IsGameThread() noexcept;

/// Player 1's field camera operator (the field root's), and a seat's own; null when none.
void* OperatorOne() noexcept;
void* Operator(int seat) noexcept;

/// The stick bank whose record `user` (a pad slot) reads: the PlayStation reader's while that slot's player plays on a
/// PlayStation pad (pad_rules::PlayingPad), else XInput's (players/pad_table). The record in either is the slot's.
std::uint32_t StickBank(std::uint32_t user) noexcept;

/// The merged stick replaced by one pad slot's record for its lifetime (`user` = the slot: 0 player 1's, 1 player 2's;
/// a pad not connected reads still), from the slot's bank (StickBank). `clearKeys` also clears the key-driven stick
/// (the keyboard is player 1's). The game
/// rebuilds the aggregate from the records every frame before the world ticks, so putting the saved values back
/// leaves the frame as the game built it. Game thread.
class StickScope
{
public:
    StickScope(std::uint32_t user, bool clearKeys) noexcept;
    ~StickScope();
    StickScope(const StickScope&)            = delete;
    StickScope& operator=(const StickScope&) = delete;

private:
    std::uint8_t* owner_ = nullptr;
    float         savedAnalog_[4]{};
    float         savedDigital_[4]{};
};

}  // namespace lss::players::seat_camera
