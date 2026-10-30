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
//      would never come (seen live: Tifa's lead stalled the Ch. 3 timer step). The member's own chain of states
//      counts too: from its own next state, the rows' next states are walked (a few links, the game's state-change
//      tables, a Stable ending a branch), and a further move among them (field_hold::MoveSetting) means the level
//      walks the member along a scripted path, every link of which the hold forecloses (seen live 2026-10-27:
//      Aerith's guide home is a run to a mark, a stay until the player comes, the next run, and on, with no Lead
//      setting in it; nothing else waited on the first run, and the chain never moved while a further player held
//      her). One move ending in a stand is a placement and stays the player's.
//   2. The story's marker (research/RE-navi-icon-target.md). The active story step's icon slot names the object the
//      objective marker rides on; a marker on the member says the game points the player at it: a guide. No code
//      links it to the LEAD setting, so it is the second signal; its absence proves nothing.
//   3. A leash on player 1 (research/RE-lead-unlock.md, players/leash_rules.h; the user's report 2026-10-29: Tifa's
//      Ch. 4 lead, player 1 turned back with the camera on her). The level writes it as an AutoWalk act (Type 56)
//      among a running state's enter actions, measured from the default leader to the member: walk-speed bands, a
//      look at the member when far, a turn-back further on. Nothing waits on the lead's arrival there, but the path
//      opens only past the member's walk, which its AI makes in single player and a held member never does. The
//      level's AutoWalk acts are found once by walking the act tables' own rows (RE-data-table-rows.md; again when a
//      table changes); a census tests only those aimed at the member against the active slots (the time is in the Info
//      line).
// Any of these makes the move needed finished. Since v0.128.0 the player keeps a needed move whose point is a field
// point (players/lead_keep_rules.h; `lead-holder keeps`): hud/lead_mark marks the point in that player's view, and
// within 2 m of it the member goes to its AI, which finishes the arrival, until the level's next setting. A needed
// move with no point to mark is a guide, as before.
// A lead or run is judged at its first sight and the judgment is logged (one Info line: the trigger, every waiter,
// the marker, the point, the verdict; again at each verdict change). A guide or at-the-mark verdict holds until the
// setting ends (its state changes, the level's next setting); a staging or keep verdict is re-read while it lasts (the
// marker and the member's distance to the point each time, the waiters once a second), so a marker that comes late
// still changes it. One judgment is kept per level object (a run-ahead is written on every member at once, one per
// further player). Reads are guarded; the game calls (the trigger table's row lookup, the story flag test, the field
// point lookup) are the ones the game makes on the same data. Game thread.

#include "players/lead_keep_rules.h"

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::players::lead_gate {

/// Resolves the globals and the story-flag test, registers the `lead.gate` reading (the last judgment), the
/// `lead.path` reading (a level state's chain walked as the gate walks it) and the `lead-holder` switch. False
/// (logged) if an engine check fails; a LEAD or a run to a mark is then staging, as before v0.26.0.
bool Install(const platform::GameImage& image) noexcept;

/// A member (`member`, its field AI `fieldAi`) runs `kind` on the level layer: the verdict from the game's data
/// (needed when a waiter waits on its arrival, the story's marker rides on it, a further move is in its own chain, or a
/// live AutoWalk act leashes player 1 to it: players/leash_rules.h) and the member's distance to the move's point.
/// Staging for a kind whose arrival the level cannot wait on (field_hold::SignalsArrival), and for a run that names no
/// level object (a state-manager command's).
lead_keep_rules::Verdict Judge(const void* member, const void* fieldAi, std::uint8_t kind) noexcept;

/// The point a kept move goes to (world centimetres) and its name.
struct Mark
{
    float        at[3] = {};
    std::int64_t point = 0;
};

/// The level has `member` on a move of its own (a LEAD or a run to a mark on the level layer, a level object's or a
/// state-manager command's), whatever the verdict and whoever walks it: world/story_boxes takes it as the story's moment for the
/// member's own story boxes (a run the level sends crosses them in single player).
bool OnLevelMove(const void* member) noexcept;

/// `member`'s current move is judged KEEP: its point. False otherwise (another verdict, no move, not judged yet).
bool KeptMark(const void* member, Mark& out) noexcept;

/// A level name's text (the log).
std::wstring PointName(std::int64_t point) noexcept;

/// hud/lead_mark installed (true) or removed: without a marker the player cannot be shown the point, so every needed
/// move is a guide, as before v0.128.0.
void SetMarking(bool on) noexcept;

/// The last judgment, one line (the reading).
std::wstring Reading() noexcept;

}  // namespace lss::players::lead_gate
