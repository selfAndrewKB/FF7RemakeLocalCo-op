#pragma once

// Which party members the game warps at a battle's start and end (research/RE-battle-warp.md).
//
// At a battle's start the game warps every present party member but the leader to a navmesh point behind the leader,
// with a short fade; at its end it warps a member back in front of the leader when it is far (by default more than 8 m,
// measured flat, after a victory; 8 m or more, or still at the battle area, after an escape). Neither reads who
// controls the member, so player 2 or 3's character is pulled away from where its player walked it. Both decisions are
// one game call each:
//   - start: the member loop fetches each member's character by type; a null answer is the game's own "no actor"
//     branch (the next member: no warp, no fade, no field action cut short). One call site redirected;
//   - end: the victory and the escape each ask whether the member needs the warp; false is the game's own "close
//     enough". Two call sites redirected.
// The setting (LocalSplitScreen.ini [Battle] battle-warps, the console's `battle-warps`, the Co-op page's "Gather at
// Battle Start/End"):
//   on   the game's warps for everyone;
//   off  every player's own character stays where it is; the game warps the members no player controls (the default).
// A member stays in the battle wherever it is: the encounter, battle area and escape tests read the leader only.
// Not changed: the warp of a member revived outside the battle area, and of a leader switched onto a
// character outside it. Logged: the setting (Info); each member the start or end decides, kept or warped,
// with its flat distance from the leader (Debug). Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::battle_warps {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::battle::battle_warps
