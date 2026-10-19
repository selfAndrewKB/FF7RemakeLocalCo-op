#pragma once

// Square Enix's directional-blur distortion apply, made to draw each view's rectangle in its own half (the streaks
// across the split line when a weapon trail crosses it).
//
// The apply pass (SE_Distortion_ApplyPass) writes the blurred distortion into SceneColor with the viewport set to the
// whole buffer, but passes DrawRectangle the view's size as the target size. Clip space is then measured in view
// heights: player 1's rectangle covers the whole screen (player 2's half gets player 1's bottom row, stretched down)
// and player 2's lands below the screen (player 2's own distortion never shows). Every other pass of the effect
// passes its target's real size. Research: RE-render-views.md 4A.1 (D3D11 capture and IDA).
//
// No game path exists (an engine bug with no setting), so this is a documented minimum fix: one instruction's operand
// changes so the target size is the buffer size the game already stores for the next argument (TextureSize). In
// single view the two sizes are equal, so nothing changes there.

namespace lss::platform {
class GameImage;
}

namespace lss::render::distortion_apply {

/// Verifies the function and the argument setup the research read, then patches. False (logged) if anything differs.
bool Install(const platform::GameImage& image) noexcept;

/// True once installed.
bool Installed() noexcept;

/// On by default; off puts the game's instruction back (for A/B checks). Any thread.
bool SetEnabled(bool enabled) noexcept;
bool Enabled() noexcept;

}  // namespace lss::render::distortion_apply
