#pragma once

// The off-screen enemy arrows (HUD slot 38: the red arrow at the view's edge pointing at an enemy outside it) placed
// through each column's own camera (RE-outside-icons-per-view.md).
//
// The game's arrow tick places every arrow once a frame through the arrow widget's player's controller (player 1's),
// so every column showed player 1's arrows. The tick reads no other player: its inputs are that controller (two
// lookups, FLocalPlayerContext::GetPlayerController), each arrow's one timer (the element's +0x14, stepped by the game
// mode's HUD step inside Place), the highlight name (+0x98, the player's last target's owner) and the host set the
// pass writes. So the game's own tick runs again per pass:
//   - a further column: the seat's timers swapped into the arrows, the seat's highlight name (its copy in
//     battle/target_cursor), the two lookups answering the seat's controller, the real step (the seat's arrows
//     fade at the game's rate); then player 1's timers and name put back;
//   - a mirror's column with a HUD pass of its own (hud/second_image.h): player 1's arrows through the mirror's
//     controller, player 1's highlight, timers of the mirror's own per column, the real step;
//   - column 0, before player 1's render once a further column ran: the tick with player 1's inputs and the HUD step
//     at 0 for that run. An arrow on screen then keeps its timer and one off screen gets the full value the tick
//     already left, so every placement, angle, visibility, alpha and picture comes out as the tick made it.
// Timers per seat (a seat keeps them when its column changes), keyed by the arrow's weak widget and the source it was
// made for (a widget reused from the pool starts at 0, as the game's new arrow does); a seat's arrows not seen in a
// frame are forgotten, and all of a seat's when its column did not run (out, no battle).
// The corner test of the one-window band layouts (the 60/120 degree split, the wrong edge near a band's
// corners) is hud/layout_code.h's third piece, installed by hud/overlay_layout.
// Game thread only (the HUD tick, the overlay driver and the redirects' installation run there).

namespace lss::platform {
class GameImage;
}

namespace lss::hud::outside_icons {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread, every tick, outside the HUD: places the two controller lookups once (the first crosses a block).
void Tick() noexcept;

/// Game thread, from the overlay driver, the column's host set in: the arrows as the column's seat sees them through
/// `controller`. A mirror's column with a pass of its own (no seat, hud/second_image.h): player 1's arrows through the
/// mirror's `controller` (its camera), player 1's highlight, timers of the mirror's own per column (stepped at the real
/// step, as a seat's). A missing controller runs nothing.
void RunForColumn(int column, void* controller) noexcept;

/// Game thread, from the overlay driver before player 1's render, player 1's host set in: the tick's own placement
/// again, if a further column ran this frame.
void RestorePlayerOne() noexcept;

/// Game thread, after the frame's overlay render: forgets the arrows a seat did not see this frame.
void EndFrame() noexcept;

}  // namespace lss::hud::outside_icons
