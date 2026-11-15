#pragma once

// Each player's own window: joining, the character, the player number, where the view is, and leaving, by pad
// (plan/topics/input.md State; the rules: players/player_window_rules.h; the text: players/player_window_text.h).
//
// A pad is its player: the pad table's slot N is player N's, so a person's own pad stays theirs. Start on a pad that
// plays no one (players/pad_table holds that Start back from the game) first takes back the lowest player whose pad is
// gone (players/pad_presence; the pad moved onto that player's slot), so a pad that died and came back on another
// XInput number plays its player again. Otherwise it adds a player on that pad: the session's join
// (the Ctrl+F7 path) on the lowest seat out, made before the session's tick (RunRequests) with the pad moved onto the
// new player's slot; the new view takes its screen by the column order, and the new player's window opens. During one
// view (a scene, a menu) the Start is remembered and joins once the views split. A join whose member is busy waits as
// Ctrl+F7's does; one with no member free is refused; both are said on player 1's window for a while.
// View held a second on a player's pad opens that player's window, in their own view only (the game's message window,
// hud/view_window; the engine-font hud/view_prompt where no window can be shown). Only that pad then reads idle to the
// game; everyone else plays on. In it, three independent changes, each a swap: the d-pad's up/down step a further
// player through the party list (the session's StepMember); its left/right trade player numbers with the nearest
// player that way (the two pads trade slots and the two screens trade numbers, so each person keeps their pad, their
// screen and their open window, and plays the other number's character; held while a join or leave is on its way);
// LB/RB move the view one screen left or right, swapped with what is there. A move or a trade changes the column order
// and saves it (the Co-op page's Screen order). A, Start, a B tap or View held again close it; B held a second takes
// player 2 or 3 out of the game (player 1 is refused, said on its window). The views collapsing into one (a scene, a
// menu) close every window.
// On the title screen a pad that presses a button becomes player 1's (its pad moved onto slot 0), so the person who
// starts the game plays player 1 on their own pad.
// Input comes from the pad table's listener, noted during the engine's pad poll and acted on in Tick. Every step plays
// the game's own menu sound for it and is logged at Info. Ctrl+F7/F8 stay. Game thread.

#include "views/column_rules.h"

#include <cstdint>

namespace lss::players {
class Session;
}

namespace lss::players::player_window {

bool Install() noexcept;
void Remove() noexcept;
/// The session whose players the windows step, name, join and take out. Set once after the install; game thread.
void SetSession(Session* session) noexcept;

/// Player 1's window shows (open, or a note): the copies' columns get HUD passes of their own, so it is not on them.
bool PlayerOneShown() noexcept;

/// The joins and leaves the windows asked for, made through the session. Once per engine tick on the game thread,
/// before the session's tick (where the keys' joins run, so the copies leave and come back on one tick); held while a
/// menu or a scene covers the screen.
void RunRequests() noexcept;

/// What the windows need of this tick's views.
struct Frame
{
    int                            screens = 1;           // the views listed (player 1's, the seats', mirrors)
    views::column_rules::ListSeats listSeats{0, -1, -1};  // the engine's list: each place's seat, -1 for none
    bool                           single  = false;       // the engine's single view is on (a scene, a menu)
    std::uint32_t                  seatIds = 1;           // the controller ids the players hold (bit 0 player 1)
};

/// Once per engine tick on the game thread, after the columns' tick.
void Tick(const Frame& frame) noexcept;

}  // namespace lss::players::player_window
