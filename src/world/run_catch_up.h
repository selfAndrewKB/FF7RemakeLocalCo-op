#pragma once

// A companion a further player walked past its own story box is brought back beside player 1's character when the
// level's run through that box goes to its AI (the user, 2026-10-30, after the Ch. 5 tunnel runs: "if the character is
// recorded as having run past the box where no member or AI was intended to go yet, maybe we can teleport the character
// back towards Cloud for the AI handoff"; research/RE-field-catchup-warp.md).
//
// The lead gate hands a run whose step moves on at the member's own armed story box to its AI (players/lead_gate
// OwnBoxAhead), so the game's run carries the member in on the game's clock. A member its player took past that box
// cannot be carried in: the run from there to its point never passes through the box (story_box_rules RunCrosses), so
// the box's held-back enter fired at the hand and the step played where the member stood (played 2026-10-30 15:54: Tifa's
// fence animation against the wrong stretch of the gate, a slow slide back). In single player the game's own follow AI
// warps a companion more than 10 m from its leader back beside it (RE §1); a held member never gets that warp. So, just
// before the hand, when the run from where the member stands misses its box and the run from beside player 1's character
// passes through it:
//   - a navmesh point 1.5 m from player 1's character toward the run's point, else 35 degrees to either side, else
//     behind it, never within 1 m of another player's companion (the game's query, its catch-up teleport's shape:
//     world/talk_gather's call);
//   - the game's battle-warp move on the member there (its party teleport, which syncs the member's own trigger sweep at
//     the landing, and a short fade-in), facing the run's point. Nothing else of the battle warp (its control-layer bits
//     can leave a field member unable to move: RE §4). The member is still its player's at that moment, its AI's move
//     refused, so the run starts from the new spot when the hand follows;
//   - the member's held-back story enters that this run passes through wait for its own body (world/story_boxes
//     AwaitRun): the game's own enter as it runs in (seen at the enter's redirect). A run that misses its box (the AI's
//     path is not the straight line) fires it at the run's point, when the run stops for 2 s, or after 20 s, late but never lost; the others fire at
//     the hand, as before.
// Nothing when the member is player 1's character, inside a player's scope, in a field action, already able to run
// through the box, when player 1's character is past it too, or when no navmesh point is found (logged; the hand goes on
// as before). Switch `run-catch-up` (on at start); reading `world.run-catch-up`. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::world::run_catch_up {

bool Install(const platform::GameImage& image) noexcept;

/// Just before the guide hand of `seat`'s member (players/session HandForHold): brings it back when the rule above holds.
/// True when moved. Game thread.
bool BeforeGuide(int seat, void* member) noexcept;

}  // namespace lss::world::run_catch_up
