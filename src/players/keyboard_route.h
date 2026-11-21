#pragma once

// The engine's half of the keyboard and mouse following the player the keyboard plays (research/RE-kbm-routing.md).
//
// The scene viewport hands every key, mouse button, wheel turn and mouse move to the game viewport client's InputKey
// and InputAxis with the Slate user as the controller id (0 for the keyboard and mouse, the mouse axes always 0); each
// finds the local player with that controller id and gives the input to that player's controller, whose own player
// input then answers the mouse look (GetInputMouseDelta). The mod takes those two entries: while the keyboard plays
// player 2 or 3 alone (players/pad_buttons KeyboardPlayer), an input that is not a pad's goes on with that player's
// controller id, so their camera turns with the mouse and player 1's reads none. Each key's repeats and release go
// where its press went (players/keyboard_route_rules). Everything after the entries is the engine's own.
// Square's own tables carry the keyboard's keys and mouse buttons through players/pad_buttons.
// Console reading: `players.keyboard-route`.

namespace lss::platform {
class GameImage;
}

namespace lss::players::keyboard_route {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::players::keyboard_route
