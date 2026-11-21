#pragma once

// Each view's button prompts in its own player's art: keycaps, Xbox, or PlayStation of the pad's own kind
// (research/RE-per-player-icons.md).
//
// Nearly every button icon the game draws is text: a `<button=NAME>` tag, turned into art when its text block is laid
// out, from its font's pad family, keyboard flag and key maps. The game keeps one answer for everyone: its device-change
// listener writes the three button fonts from the last device used (player 1's while views are split: the other
// players' pads are kept out of that note, and so is the keyboard while it plays player 2 or 3, players/pad_buttons).
// Player 1's view therefore shows the game's own art, and each other player's HUD pass shows theirs:
//   - before the pass, when the seat's art differs from the game's, the game's own listener writes the seat's art into
//     the fonts (given the seat's device bytes, its closing relayout of every text skipped), and every live text block
//     that holds a button tag gets the seat's own laid-out state swapped in (hud/label_states: player 1's set aside);
//   - a block whose seat state is new, or was laid out for another text, font or art, is laid out at once by the game's
//     relayout with the seat's fonts, sized as player 1's state was, so the pass's prepass already sees its size: the
//     art is the game's own relayout, made once per change, never per frame;
//   - after the pass, the fonts get the game's bytes back and player 1's states go back in; a button block laid out in
//     the pass without a seat state of its own is laid out again for player 1.
// While the screen shows one view (menus, minigames, scenes) no pass runs: every view is the game's, and every player's
// device moves the game's art as in single player.
// Logged: each player's art on change (Info), a census of the button blocks and the passes' counts (Debug). Console:
// `button-art` (on/off), reading `players.button-art`. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::button_art {

bool Install(const platform::GameImage& image) noexcept;

/// Player 1's state back in every block, every record freed.
void Remove() noexcept;

/// Before a further column's HUD render, for the seat it shows (0: player 1's content, a mirror or a copy: nothing).
void EnterPass(int seat) noexcept;

/// After that render.
void LeavePass() noexcept;

/// Once per engine tick, outside the passes: the records of dead blocks dropped, the census lines.
void Tick() noexcept;

}  // namespace lss::hud::button_art
