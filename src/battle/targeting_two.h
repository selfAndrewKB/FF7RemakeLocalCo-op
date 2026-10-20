#pragma once

// Player 2's targeting: the game's own targeting tick run a second time, for player 2 (RE-battle-member-two.md §5,
// RE-battle.md §2.9; Rebirth's player_two/lock_on, the same shape).
//
// The battle system keeps one set of target fields (the lock, the displayed target, the lock-camera target, the
// target-list mode, the stick-switch latch, the dummy-lock option and timer). Its targeting tick (the lock toggle,
// the dummy-lock camera, the target-list mode, the locked follow or the stick switch, the displayed target) runs
// from two calls in the battle tick, each behind the game's own gates, right after a pre-tick that gives the leader
// its soft target and before the cursor widget's tick.
//
// Both calls are redirected (the second crosses a code block: placed from the game thread). The game's call runs
// for player 1; then the pre-tick and the targeting tick run again for player 2 inside player 2's scopes: the
// leader and the primary (the lock camera and the stick flag), the camera operator (the lock state and the target
// flavour land on player 2's camera), the UI slot and focus context (the menu's target row), pad 2's buttons and
// right stick, and player 2's own copy of the target fields exchanged into the battle system. Player 2's lock
// toggle posts the game-wide Global.LockOn, so player 1's lock is put back through the game's setter before the
// copy comes out. The game's cursor driver runs again for player 2 right after (battle/target_cursor_two.h). At the battle's end
// (the battle HUD mode falling to 0) player 2's lock and target are ended with the game's own clear, on its copy.
// Game thread only.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::targeting_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: places the crossing redirect once; ends player 2's targeting when a battle
/// ends or the battle system goes.
void Tick() noexcept;

/// Player 2 leaves: its lock and target ended.
void OnLeave() noexcept;

/// Player 2's lock byte from its copy (0 when it has none).
std::uint8_t PlayerTwoLock() noexcept;

/// Player 2's target fields in the battle system while it lives (nothing outside a battle, or before player 2's
/// first targeting run): for every game call made for player 2 that reads or writes the lock, the displayed target
/// or the target-list mode (the targeting run, the operator tick, the input and movement steps, the command
/// controller's tick, the re-targets). If the call changed player 2's lock, player 1's goes back through the game's
/// setter before the fields come out, so the game-wide Global.LockOn is player 1's again; player 2's lock and
/// dummy-lock latch stay in its copy.
class FieldsScope
{
public:
    FieldsScope() noexcept;
    ~FieldsScope();
    FieldsScope(const FieldsScope&)            = delete;
    FieldsScope& operator=(const FieldsScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    void*        system_  = nullptr;
    std::uint8_t lockOne_ = 0;  // player 1's lock when the fields went in
    bool         held_    = false;
};

}  // namespace lss::battle::targeting_two
