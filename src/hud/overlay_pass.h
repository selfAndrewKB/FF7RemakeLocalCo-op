#pragma once

// Which player's pass of the HUD overlay's render is running (hud/second_image.h sets it, the overlay's redirected
// calls read it: hud/overlay_layout.h). Game thread only.

namespace lss::hud::overlay_pass {

enum class Pass
{
    Single,     // the game's own one render: no second image this frame
    PlayerTwo,  // player 2's render, first in the frame
    PlayerOne,  // player 1's render, after player 2's: the game's own call
};

Pass Current() noexcept;
void Set(Pass pass) noexcept;

}  // namespace lss::hud::overlay_pass
