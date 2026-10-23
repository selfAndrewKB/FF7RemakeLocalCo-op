#pragma once

// Each seat's targeting: the game's own targeting tick run again, once per seat (RE-battle-member-two.md §5,
// RE-battle.md §2.9; Rebirth's player_two/lock_on, the same shape, single-seat; the seats follow plan/seats.md step 3).
//
// The battle system keeps one set of target fields (the lock, the displayed target, the lock-camera target, the
// target-list mode, the stick-switch latch, the dummy-lock option and timer). Its targeting tick (the lock toggle,
// the dummy-lock camera, the target-list mode, the locked follow or the stick switch, the displayed target) runs
// from two calls in the battle tick, each behind the game's own gates, right after a pre-tick that gives the leader
// its soft target and before the cursor widget's tick.
//
// Both calls are redirected (the second crosses a code block: placed from the game thread). The game's call runs
// for player 1; then, for each built seat whose player holds a member, the pre-tick and the targeting tick run again
// inside the seat's scopes: the leader and the primary (the lock camera and the stick flag), the camera operator
// (the lock state and the target flavour land on the seat's camera), the UI slot and focus context (the menu's
// target row), the seat's pad's buttons and right stick, and the seat's own copy of the target fields exchanged
// into the battle system. A seat's lock toggle posts the game-wide Global.LockOn, so player 1's lock is put back
// through the game's setter before the copy comes out. The game's cursor driver runs again right after
// (battle/target_cursor_two.h, the seat's own cursor). At the battle's end (the battle HUD
// mode falling to 0, or the battle system changing) every seat's lock and target are ended with the game's own
// clear, on its copy.
// Game thread only.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::targeting_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: places the crossing redirect once; ends every seat's targeting when a
/// battle ends or the battle system goes.
void Tick() noexcept;

/// The seat's player leaves: its lock and target ended.
void OnLeave(int seat) noexcept;

/// The seat's lock byte from its copy (0 when it has none).
std::uint8_t Lock(int seat) noexcept;

/// The seat's target fields in the battle system while it lives (nothing outside a battle, or before the seat's
/// first targeting run): for every game call made for the seat that reads or writes the lock, the displayed target
/// or the target-list mode (the targeting run, the operator tick, the input and movement steps, the command
/// controller's tick, the re-targets). If the call changed the seat's lock, player 1's goes back through the game's
/// setter before the fields come out, so the game-wide Global.LockOn is player 1's again; the seat's lock and
/// dummy-lock latch stay in its copy.
class FieldsScope
{
public:
    explicit FieldsScope(int seat) noexcept;
    ~FieldsScope();
    FieldsScope(const FieldsScope&)            = delete;
    FieldsScope& operator=(const FieldsScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    int          seat_    = 0;
    void*        system_  = nullptr;
    std::uint8_t lockOne_ = 0;  // player 1's lock when the fields went in
    bool         held_    = false;
};

}  // namespace lss::battle::targeting_two
