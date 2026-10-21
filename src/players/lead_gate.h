#pragma once

// Whether a LEAD the level wrote on a further player's member is a guide (the member goes to its AI, the player's
// view stays on it) or staging (the player keeps it), from the game's own data (the user, 2026-10-21).
//
// The LEAD runner is the same code for both; the level data differs. Two of the game's records say which:
//   1. The level's waiters (research/RE-lead-arrival-wait.md §3). The Lead setting's state row names an arrival
//      trigger; the level state manager's active slots hold every level object's current state row, and a row lists
//      the triggers it waits on. The member's own AI step always waits on its arrival (Lead to Stable); any OTHER
//      slot waiting on it (a story manager's step, a timer, a door), or the trigger row's own direct state sets on
//      another object, means the level cannot advance until the member arrives: a guide. A held member's runner
//      never ticks, so that arrival would never come (seen live: Tifa's lead stalled the Ch. 3 timer step).
//   2. The story's marker (research/RE-navi-icon-target.md). The active story step's icon slot names the object the
//      objective marker rides on; a marker on the member says the game points the player at it: a guide. No code
//      links it to the LEAD setting, so it is the second signal; its absence proves nothing.
// A lead is judged at its first sight and the judgment is logged (one Info line: the trigger, every waiter, the
// marker, the verdict). A guide verdict holds until the lead ends (the level's next setting); a staging verdict is
// re-read while the lead lasts (the marker each time, the waiters once a second), so a marker that comes late
// still hands the member over. Reads are guarded; the two game calls (the trigger table's row lookup, the story
// flag test) are the ones the game makes on the same data. Game thread.

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::players::lead_gate {

/// Resolves the globals and the story-flag test, registers the `lead.gate` reading (the last judgment). False
/// (logged) if an engine check fails; a LEAD is then staging, as before v0.26.0.
bool Install(const platform::GameImage& image) noexcept;

/// `member`'s field AI (`fieldAi`) runs a LEAD on the level layer: true if the game's data makes it a guide.
bool Guide(const void* member, const void* fieldAi) noexcept;

/// The last judgment, one line (the reading).
std::wstring Reading() noexcept;

}  // namespace lss::players::lead_gate
