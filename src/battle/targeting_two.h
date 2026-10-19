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
// copy comes out. The cursor widget's tick is not run for player 2 (one cursor, player 1's). At the battle's end
// (the battle HUD mode falling to 0) player 2's lock and target are ended with the game's own clear, on its copy.
// Game thread only.

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

/// Player 2's target fields in the battle system while it lives (nothing outside a battle, or before player 2's
/// first targeting run): for player 2's camera operator tick, which follows the lock-camera target.
class FieldsScope
{
public:
    FieldsScope() noexcept;
    ~FieldsScope();
    FieldsScope(const FieldsScope&)            = delete;
    FieldsScope& operator=(const FieldsScope&) = delete;

private:
    bool held_ = false;
};

}  // namespace lss::battle::targeting_two
