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
// what is left), or, while a screen is open, when every player's screen is kept; View held again puts back the pads
// and the characters from the opening (or the last join or leave). The views collapsing into one (a scene, a menu)
// ends it with what is chosen so far.
// Joining and leaving: a pad that plays no one (a spare pad: its slot holds no player; the game never sees its
// Start, players/pad_table) joins with an open screen's button (a copy's screen, "NEW PLAYER", shown in the copy's
// own HUD pass: the copies get passes of their own while the claim is open) or with Start (the first open screen;
// in one window a new view). Start with the claim shut opens it onto that join, the players already in kept; during
// one view (a scene, a menu) it is remembered and joins once the views split. The join is the session's (the
// Ctrl+F7 path) on the lowest seat out, made before the session's tick (RunRequests) with the joiner's pad on the
// new player's slot and the column order showing it on the screen chosen; the claim then goes on over the new
// screens, the joiner on its screen with its character (the next free member). A join with no member free is refused
// and said on that screen; one whose member is busy waits as Ctrl+F7's does. B held a second on a further player's
// pad takes that player out; player 1's is refused on its screen. Ctrl+F7/F8 stay.
// Input comes from the pad table's listener, noted during the engine's pad poll and acted on in Tick. Every step plays
// the game's own menu sound for it and is logged at Info. Game thread.

#include "views/column_rules.h"

#include <cstdint>

namespace lss::players {
class Session;
}

namespace lss::players::pad_claim {

bool Install() noexcept;
void Remove() noexcept;
/// The session whose players the claim steps, names, joins and takes out. Set once after the install; game thread.
void SetSession(Session* session) noexcept;

/// The claim is open: the copies' columns get HUD passes of their own (their prompts are their own).
bool Active() noexcept;

/// The joins and leaves the claim asked for, made through the session. Once per engine tick on the game thread, before
/// the session's tick (where the keys' joins run, so the copies leave and come back on one tick); held while a menu
/// or a scene covers the screen.
void RunRequests() noexcept;

/// What the claim needs of this tick's views.
struct Frame
{
    int                                screens = 1;          // the views listed (player 1's, the seats', mirrors)
    views::column_rules::ListSeats     listSeats{0, -1, -1};  // the engine's list: each place's seat, -1 for none
    bool                               single = false;        // the engine's single view is on (a scene, a menu)
    std::uint32_t                      seatIds = 1;           // the controller ids the players hold (bit 0 player 1)
};

/// Once per engine tick on the game thread, after the columns' tick.
void Tick(const Frame& frame) noexcept;

}  // namespace lss::players::pad_claim
