#pragma once

// Neither player switches onto a member the other human plays (RE-battle-switch.md §4-§5; Rebirth's
// player_two/party_switch, with the turned filter its research proposed).
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
// In co-op the game's one forced switch is refused (the user, 2026-10-28: a downed player stays on their character
// until revived): the KO'd leader's dead-state tick calls the cycle with "forced" every tick, and that call returns at
// once while a further player holds a member, so player 1 stays on the downed leader even with an AI ally standing
// (research/RE-pyramid-trap-control.md; a Pyramid's trap moves no one's control itself). battle/enemy_leader then
// turns the enemies on the member the switch would have taken. Players 2 and 3 are never auto-switched (their
// member's KO tick fails the leader test). Single player keeps the game's switch.
// CanSwitch's call and the KO tick's call cross code blocks, so they are placed from the game thread outside the
// functions. Refusals are logged on change (a downed episode's start, and its end after 30 quiet ticks). The game
// thread [ASSUMPTION for the dead-state tick, a component tick; the hook's one flag is atomic].

namespace lss::platform {
class GameImage;
}

namespace lss::battle::party_switch {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: places the crossing redirect once.
void Tick() noexcept;

}  // namespace lss::battle::party_switch
