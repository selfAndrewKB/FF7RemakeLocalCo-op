#pragma once

// A few lines of text in one player's own view: the pad claim's prompts where the game's own window cannot be shown
// (players/pad_claim, hud/view_window; research/RE-view-text.md).
//
// The engine's own per-player HUD pass draws them: UGameViewportClient::Draw hands each local player's HUD a canvas
// already offset to that player's view, above the game's HUD, and calls its PostRender through the AHUD vtable (every
// HUD here is the plain engine class, one table). The mod's PostRender calls the game's, then, while a prompt is set
// for the HUD's player, draws a translucent box (the HUD's own reflected DrawRect) and the lines (the engine's
// FCanvas::DrawShadowedString, in the engine's medium font, Roboto: the game's own fonts are not UFonts and cannot be
// drawn there). The HUD's player is its controller's seat (players/seat_objects), else player 1 (a mirror's HUD shows
// player 1's picture). Nothing is drawn while no prompt is set. A view the engine does not draw (the single view of a scene
// or a menu: only player 1's) shows no prompt. Reading `views.prompt`. Game thread.

#include <array>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::view_prompt {

inline constexpr int kLines = 6;

struct Prompt
{
    std::array<std::wstring, kLines> lines;          // top to bottom; an empty line is skipped
    int                              highlight = -1;  // the line drawn in the highlight colour, or -1
};

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Shows `prompt` in `seat`'s view (0 player 1's) until replaced or cleared. Game thread.
void Show(int seat, const Prompt& prompt) noexcept;

/// No prompt in any view. Game thread.
void ClearAll() noexcept;

}  // namespace lss::hud::view_prompt
