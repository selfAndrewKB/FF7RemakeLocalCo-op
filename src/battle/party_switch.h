#pragma once

// Neither player switches onto a member the other human plays (RE-battle-switch.md §4-§5; Rebirth's
// player_two/party_switch, with the turned filter its research proposed).
//
// The game's battle switch walks the deployed members from the leader, one step either way with wrap-around,
// and takes the first whose character it can fetch by type, has its battle component and is not knocked out or
// away; CanSwitch counts the members passing the same test before a manual switch. Both fetch each candidate
// with the party manager's actor-by-type call, and a null answer is the game's own "skip this member" branch.
// Those two calls are redirected: outside player 2's tick the member player 2 plays answers null, so player 1's
// d-pad switch and the game's KO auto-switch pass over it; inside player 2's tick (its own controller's switch
// step, run with player 2 as the leader) player 1's leader answers null instead, so player 2 may take any member
// no human holds, the field leader included, and never player 1's. With player 2's member the last one standing,
// nothing passes and player 1 stays on the downed leader, as the game keeps a downed leader with no one to take.
// CanSwitch's call crosses a code block, so it is placed from the game thread outside the function. Refusals are
// logged on change. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::party_switch {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: places the crossing redirect once.
void Tick() noexcept;

}  // namespace lss::battle::party_switch
