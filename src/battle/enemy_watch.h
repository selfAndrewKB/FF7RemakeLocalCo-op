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
// Who aims an enemy, and when it is ready but holds back, as Laser Cannons idling on far players do
// (research/RE-enemy-attack-cadence.md §6, live check L1), from two entry redirects that call the game's function
// unchanged (the ready test's is battle/enemy_stall's, which hands each answer here):
//   - BattleComp_SetTargetChar, every target call into an enemy's component (party members' calls pass untouched):
//     Debug, who called it (up to three frames, the known callers named: the hate update, the script's SetTarget /
//     SetTargetByType, the battle's start, the leader change), the character asked for, the one it holds after the
//     call (a hate enemy's call takes its hate pick instead) and before it, and how far the enemy is from it and from
//     each human's member. A target change the polled watch sees with no such call in the last 250 ms is marked as
//     made by a path the call does not cover;
//   - the attack-ready test the enemy component asks each tick (vtable +0x1A8; on 1 the game fires the Blueprint's
//     OnATBLightAttackEvent, on 2 OnATBHardAttackEvent): Debug when an enemy becomes ready (light, or heavy with its
//     ATB full) with its target and the distances, every 5 s then 10 s while it stays ready without acting, and in
//     the action's start line how long it was ready. A ready enemy that never acts on a far target is the case.
// Distances are 3D, between the characters' root components, in metres. The hooks run on any thread: they read the
// humans' members from copies the game thread refreshes each tick. Runs only while the log writes Debug lines (the
// reading says so otherwise); each gate call then costs two guarded calls and two guarded reads, each ready test one
// table scan.

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::enemy_watch {

/// Whether the gate should hand enemies here. Any thread.
bool Watching() noexcept;

/// An enemy's gate call: its controller, its battle component and its running action (0: none). Any thread.
void Observe(void* controller, const void* component, std::uint64_t action) noexcept;

/// An enemy component's attack-ready answer (0, 1 light, 2 heavy), from battle/enemy_stall's redirect. Any thread.
void Ready(const void* component, char ready) noexcept;

/// The character a battle component targets (its own target pair, read without taking a reference); null if none.
/// The game thread between AI ticks, or the battle AI thread that writes the pair.
void* TargetCharacter(const void* component) noexcept;

/// "<its target> at 8.2 m; player 2's member at 23.1 m": an enemy component's target and the humans' members, by
/// distance from the enemy, for a log line. Allocates; never throws.
std::wstring TargetAndDistances(void* component) noexcept;

/// Finds the game's controller-to-component getter, places the target-call redirect (left out if it differs, logged)
/// and registers the reading. False (logged) if the getter differs.
bool Install(const platform::GameImage& image) noexcept;

/// Puts the redirect back.
void Remove() noexcept;

/// Game thread, once per frame: names and logs what changed.
void Tick() noexcept;

}  // namespace lss::battle::enemy_watch
