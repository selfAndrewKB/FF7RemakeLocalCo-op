#pragma once

// What each enemy is doing, for the live reads behind the circling report (an enemy endlessly circling player 2's
// member without attacking: plan/open-items.md, research/RE-enemy-circling.md). Logging only: nothing is written to
// the game's state (making a weak pointer may give the controller its serial number, the engine's own lazy write).
//
// The coin's decision gate (battle/enemy_coin), which the battle AI tick calls for every enemy on every tick, hands
// each enemy's controller and battle component here with its running action (the game's action field, an FName; 0
// while the enemy is between actions) and its target object (the component's own target, a shared-pointer pair).
// Any thread records into a small table, claiming an entry by a compare-and-swap; only the gates claim, so the game
// thread never writes an entry. The game thread takes an enemy at first sight only once the object array confirms a
// live battle AI controller at that address (a write-free check), then holds it by a weak pointer and reads the
// component again through the game's own getter from the still-live controller, so nothing is read from an enemy the
// engine has destroyed. It names what it holds and logs what changed:
//   - Debug: an enemy starts an action (its name and the target), ends one (after how long), changes target during
//     one, has started no action for 5 s (again every 10 s while it waits), is gone (destroyed), or was not seen for a
//     second (it left the battle, or the game paused);
//   - Debug: each human's member's battle area (the points of its first area query, which enemies read around the
//     character they target: RE-enemy-circling.md) at each change between having points, empty and unreadable, read
//     once a second while enemies are seen;
//   - `enemy-watch` reads every enemy seen in the last second: its action, for how long, and its target; then each
//     human's member's area points.
// Runs only while the log writes Debug lines (the reading says so otherwise); each gate call then costs two guarded
// calls and two guarded reads.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::enemy_watch {

/// Whether the gate should hand enemies here. Any thread.
bool Watching() noexcept;

/// An enemy's gate call: its controller, its battle component and its running action (0: none). Any thread.
void Observe(void* controller, const void* component, std::uint64_t action) noexcept;

/// Finds the game's controller-to-component getter and registers the reading. False (logged) if the getter differs.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, once per frame: names and logs what changed.
void Tick() noexcept;

}  // namespace lss::battle::enemy_watch
