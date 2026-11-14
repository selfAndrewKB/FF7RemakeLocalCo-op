#pragma once

// What the field encounter sees, for the option "enemies notice players 2 and 3" (research/RE-field-battle-start-seats.md).
// Logging only: nothing of the game's changes.
//
// The game's per-group encounter step (EncountGroup_TickStateMachine, once per group from the field encounter tick) runs
// through an entry redirect that calls it unchanged and compares the group's state before and after. Each group watches
// only the party leader (player 1's character): it turns to caution when an enemy sees the leader, engages when the leader
// stands in the group's battle area, and escapes when the leader leaves it. The watch asks the game's own area test
// (EncountMgr_IsCharInBattleAreaOfGroup, which reads each character's own nav-area record) the same question for every
// player's character, so the log shows what a group would see if it watched player 2 or 3, and whether their area
// records follow them at all.
//   - Info: a group's state change (idle, caution, engage, battle, escaping, escaped, ended), with which players'
//     characters stand in its battle area at that moment;
//   - Debug: a player's character entering or leaving a group's battle area (each group asked at most twice a second);
//   - `encounter-watch` reads the groups seen in the last second: their state and who stands in their area, or why
//     they wait.
// A group steps only once its lifecycle is 1: every counted member's actor spawned, with its battle component, and not
// hidden by reason 11 or 14 (else it waits at 0, its enemies roaming on their own field AI). The group's update, which
// runs for every group every tick, goes through a second entry redirect, also unchanged; while a group waits (read at
// most once a second) the watch reads the same test from the game's own member getter and hide test:
//   - Info: player 2 or 3 within 15 m of a waiting group's enemy (once, again after 25 m away), with why the group waits
//     (the first enemy not spawned, without its battle component, or hidden; or every enemy shown: the group is
//     switched off by its scene object's state) and how far player 1 is;
//   - Debug: a group's lifecycle change, and a waiting group's reason when it changes.
// Game thread only: the update and step run on it, and the redirects are placed from the engine tick, outside them.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::encounter_watch {

/// Resolves the two functions and registers the reading. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: places the redirect once (from outside the step).
void Tick() noexcept;

}  // namespace lss::battle::encounter_watch
