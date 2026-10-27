#pragma once

// Whether a LEAD or a run to a mark (MoveToTarget) the level wrote on a further player's member is a guide (the
// member goes to its AI, the player's view stays on it) or staging (the player keeps it), from the game's own data
// (the user, 2026-10-21; the run to a mark added 2026-10-27 after Aerith's run ahead of Cloud stalled under a further
// player).
//
// Each kind's runner is the same code whether the level stages it or waits on it; the level data differs. Both
// kinds signal the level on arrival through the same path (research/RE-fieldai-kinds.md §2, §3), so the same two of
// the game's records say which:
//   1. The level's waiters (research/RE-lead-arrival-wait.md §3). The setting's state row (Lead or MoveToTarget)
//      names its arrival trigger (string argument 2); the level state manager's active slots hold every level
//      object's current state row, and a row lists the triggers it waits on. The member's own AI step always waits
//      on its arrival (the setting to its next, e.g. Lead to Stable); any OTHER slot waiting on it (a story
//      manager's step, a timer, a door), or the trigger row's own direct state sets on another object, means the
//      level cannot advance until the member arrives: a guide. A held member's runner never ticks, so that arrival
//      would never come (seen live: Tifa's lead stalled the Ch. 3 timer step). The member's own next state counts
//      too when it is another arrival setting (the game's list, field_hold::ArrivalSetting): the level walks the
//      member on along a scripted path (seen live 2026-10-27: Aerith's run to a mark, then her lead to her house;
//      nothing else waited on the run, and the lead never came while a further player held her).
//   2. The story's marker (research/RE-navi-icon-target.md). The active story step's icon slot names the object the
//      objective marker rides on; a marker on the member says the game points the player at it: a guide. No code
//      links it to the LEAD setting, so it is the second signal; its absence proves nothing.
// A lead or run is judged at its first sight and the judgment is logged (one Info line: the trigger, every waiter,
// the marker, the verdict). A guide verdict holds until the setting ends (its state changes, the level's next
// setting); a staging verdict is re-read while it lasts (the marker each time, the waiters once a second), so a
// marker that comes late still hands the member over. One judgment is kept per level object (a run-ahead is
// written on every member at once, one per further player). Reads are guarded; the two game calls (the trigger
// table's row lookup, the story flag test) are the ones the game makes on the same data. Game thread.

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::players::lead_gate {

/// Resolves the globals and the story-flag test, registers the `lead.gate` reading (the last judgment). False
/// (logged) if an engine check fails; a LEAD or a run to a mark is then staging, as before v0.26.0.
bool Install(const platform::GameImage& image) noexcept;

/// A member's field AI (`fieldAi`) runs `kind` on the level layer: true if the game's data makes it a guide. False
/// for a kind whose arrival the level cannot wait on (field_hold::SignalsArrival), and for a run that names no level
/// object (a state-manager command's).
bool Guide(const void* fieldAi, std::uint8_t kind) noexcept;

/// The last judgment, one line (the reading).
std::wstring Reading() noexcept;

}  // namespace lss::players::lead_gate
