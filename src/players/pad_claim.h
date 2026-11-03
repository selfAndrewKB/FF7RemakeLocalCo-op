#pragma once

// The claim screen: which pad plays which character, chosen by the people holding them (plan/topics/input.md State,
// "Live pad assignment"; the user's design of 2026-10-30 18:43, approved; the rules: players/pad_claim_rules.h).
//
// View held a second on any pad opens it, while the views are split (or player 1 is alone). Every connected pad then
// reads idle to the game (players/pad_table), and each screen shows its own prompt in the game's message window
// (hud/view_window; the text: players/pad_claim_text.h; the engine-font hud/view_prompt where no window can be shown):
// the screen first, then the character. A screen no pad took names its player and its button ("PLAYER 2", the first
// screen A, the second X, the third Y; in one window the top, then the bottom left and right), as the game's button
// icons. A press of it makes the pressing pad that screen's player (a swap in the pad table), and the screen shows the
// character. On a further player's screen the d-pad left/right steps to the next party member no other human plays
// (the session's StepMember: the player's rank, its member given back and the new one seated); player 1's character
// is the game's leader. A keeps the choice, B steps back. It ends when all screens but one are kept (the last takes
// what is left); View held again puts back the pads and the characters from the opening. The views
// collapsing into one (a scene, a menu) or a player joining or leaving ends it with what is chosen so far.
// Input comes from the pad table's listener, noted during the engine's pad poll and acted on in Tick. Every step plays
// the game's own menu sound for it and is logged at Info. Game thread.

#include "views/column_rules.h"

namespace lss::players {
class Session;
}

namespace lss::players::pad_claim {

bool Install() noexcept;
void Remove() noexcept;
/// The session whose players the claim steps (and names). Set once after the install; game thread.
void SetSession(Session* session) noexcept;

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
