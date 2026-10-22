#pragma once

// Enemies follow the human whose member stands when the game's leader is down (RE-enemy-leader-reads.md §2, §5.1).
//
// The game's leader is one character. Enemies whose battle-AI spec says "follow the leader" (TargetSelectType 10)
// are re-targeted by the game's forced leader switch: when the leader is knocked out, the KO auto-switch picks the
// next standing member, writes the leader, then walks the character registry setting every such enemy's target to
// the new leader with the battle component's own setter; on the next tick every battle AI controller compares its
// remembered leader with the party leader and fires its Blueprint OnChangeLeader. In co-op the switch may not land
// on a member another human plays (battle/party_switch), so with player 1's leader down and only player 2's member
// standing the cycle finds nobody: no re-target, no leader-change event, and an enemy keeps hitting the downed
// leader, as Rude did (nothing in the game checks that a non-hate enemy's target is alive) [CONFIRMED live].
//
// The co-op result is the single-player one: the enemies turn on the standing human's member. The leader itself
// stays (player 1 inert on its downed leader, the user's rule), so the two steps the switch would have run get the
// member the game would have switched to:
//   - the enemy-facing leader: the game's leader while it stands, else the first other human's member that stands,
//     else the game's own leader; read each engine tick on the game thread while a battle runs;
//   - when it changes to a standing member that is not the game's leader, or back to the game's leader without a
//     switch (the member went down, or the leader revived), the game's re-target tail runs once with that member:
//     the registry walk with the game's iterator (category bit 2), the game's predicate (a battle component not in
//     the KO state, a battle AI whose spec says 10), the game's setter. Ten lines of the cycle recreated with the
//     game's own functions: the game has no path to that step without a leader write, which the rule forbids;
//   - the battle AI tick's leader read (its one call site) answers that member for every battle AI while the
//     enemy-facing leader differs from the game's, so the game's own OnChangeLeader fires, forward and back.
// The Blueprint natives an enemy script picks "the player" with (SetTargetByType(UCPC), GetLeader, the target-type
// collector behind GetActionCharacter and kin; none tests that the leader stands) were answered here too from
// v0.19.1 (the re-target alone left Rude's script re-picking the downed Cloud each action [CONFIRMED live]); since
// v0.36.0 battle/enemy_coin answers them from each enemy's coin, whose toss skips a downed member.
// Game thread only: a read off it gets the game's answer and is counted. Logged on each change (Debug) with the
// number of enemies re-targeted; `enemy-leader` reads the state.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::enemy_leader {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: the enemy-facing leader, and the re-target when it changes.
void Tick() noexcept;

}  // namespace lss::battle::enemy_leader
