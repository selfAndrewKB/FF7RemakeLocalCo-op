#pragma once

// The claim screen: which pad plays which character, chosen by the people holding them (plan/topics/input.md State,
// "Live pad assignment"; the user's design of 2026-10-30 18:43, approved; the rules: players/pad_claim_rules.h).
//
// View held a second on any pad opens it, while the views are split (or player 1 is alone). Every connected pad then
// reads idle to the game (players/pad_table), and each screen shows its own prompt in the game's message window
// (hud/view_window; the text: players/pad_claim_text.h; the engine-font hud/view_prompt where no window can be shown):
// the character there and that screen's button (the first A, the second X, the third Y; in one window the top, then
// the bottom left and right), as the game's button icons. A press of it gives the pressing pad that screen's
// character (a swap in the pad table). On a
// screen it took, the d-pad left/right steps through the characters: the view of the next character not kept swaps
// screens with this one (the column order, views/column_layout) and the pads follow, so the person keeps pad and
// screen; A keeps the choice, B steps back. It ends when all screens but one are kept (the last takes what is left);
// View held again puts back the pads and screens from the opening. The views collapsing into one (a scene, a menu) or
// a player joining or leaving ends it with what is chosen so far. Any player may take any screen, player 1 too (with
// two players through the engine's two-player entries).
// Input comes from the pad table's listener, noted during the engine's pad poll and acted on in Tick. Every step plays
// the game's own menu sound for it and is logged at Info. Game thread.

#include "views/column_rules.h"

namespace lss::players::pad_claim {

bool Install() noexcept;
void Remove() noexcept;

/// What the claim needs of this tick's views.
struct Frame
{
    int                                screens = 1;          // the views listed (player 1's, the seats', mirrors)
    views::column_rules::ListSeats     listSeats{0, -1, -1};  // the engine's list: each place's seat, -1 for none
    bool                               single = false;        // the engine's single view is on (a scene, a menu)
};

/// Once per engine tick on the game thread, after the columns' tick.
void Tick(const Frame& frame) noexcept;

}  // namespace lss::players::pad_claim
