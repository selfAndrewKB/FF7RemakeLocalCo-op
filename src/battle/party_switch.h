#pragma once

// Neither player switches onto a member the other human plays (research/RE-battle-switch.md).
//
// The game's battle switch walks the deployed members from the leader, one step either way with wrap-around,
// and takes the first whose character it can fetch by type, has its battle component and is not knocked out or
// away; CanSwitch counts the members passing the same test before a manual switch. Both fetch each candidate
// with the party manager's actor-by-type call, and a null answer is the game's own "skip this member" branch.
// Those two calls are redirected: outside a seat's tick every member a further player plays answers null, so player
// 1's d-pad switch and the game's KO auto-switch pass over them; inside a seat's tick (its own controller's switch
// step, run with that player as the leader) player 1's leader and every other seat's member answer null instead, so
// the player may take any member no human holds, the field leader included, and never another human's. With the
// other players' members the last ones standing, nothing passes and player 1 stays on the downed leader, as the game
// keeps a downed leader with no one to take.
// In co-op the game's one forced switch is refused, so a downed player stays on their character until revived: the
// KO'd leader's dead-state tick calls the cycle with "forced" every tick, and that call returns at
// once while a further player holds a member, so player 1 stays on the downed leader even with an AI ally standing
// (research/RE-pyramid-trap-control.md; a Pyramid's trap moves no one's control itself). battle/enemy_leader then
// turns the enemies on the member the switch would have taken. Players 2 and 3 are never auto-switched (their
// member's KO tick fails the leader test). Single player keeps the game's switch.
// A player's own switch in a fight is the game's: a seat's d-pad reaches its own command controller's switch step,
// ticked inside the seat's scopes (battle/command_menu_tick.h), so the game's cycle starts at the seat's member, passes
// over the other humans' as above, hands the old member to its battle AI and possesses the new one with the seat's
// controller, its camera retargeted by the game's own broadcast; the session hears the change inside that tick, adopts
// the member and keeps it while the fight lasts (players/session.h, battle/party_switch_rules.h). The seat's tick holds
// its Operator scope, so the cycle's camera-transition gate reads the seat's own camera.
// A downed player switches too: the d-pad's two calls of the cycle are redirected, and in co-op, for a leader whose
// battle component lies knocked out, the cycle runs as the game's own forced switch runs it (its two-standing gate
// skipped), since the mod refuses that forced switch and the gate, which does not count the other players' members,
// would leave a downed player with no switch. Run so, the cycle also turns the AI members that follow the leader onto
// the new one, as the game's forced switch does. Logged per press (Debug), at Info when the gate is dropped.
// CanSwitch's call, the KO tick's call and the d-pad's two calls cross code blocks, so they are placed from the game
// thread outside the functions. Refusals are logged on change (a downed episode's start, and its end after 30 quiet
// ticks). The game thread [ASSUMPTION for the dead-state tick, a component tick; the hook's one flag is atomic].

namespace lss::platform {
class GameImage;
}

namespace lss::battle::party_switch {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: places the crossing redirects once.
void Tick() noexcept;

/// The seat (1..) whose d-pad press runs the game's cycle now, inside its command tick; 0 outside one and for player
/// 1's press. The session hears the leader change the cycle makes as that seat's own switch. Game thread.
int CyclingSeat() noexcept;

}  // namespace lss::battle::party_switch
