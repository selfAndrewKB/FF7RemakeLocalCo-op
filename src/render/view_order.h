#pragma once

// A test for the blue haze in player 2's and player 3's views (RE-haze-occlusion.md, Pass 9 LF1): the order of the
// views in the family the game renders each frame. All players' views go to the renderer as one family; the haze is
// only ever in a view after the first, and only while that view hides something by occlusion. Swapping the first two
// views tells whether it follows the place in the family or the player: each view keeps its own rectangle, camera
// and view state, so the picture does not move.
//
// Switch `view-order stock | swap`: swap = the family's second view (player 2's while player 2 is in; a mirror of
// player 1's picture while the window is spanned without player 2) goes first, the first second; a third view stays
// third. Every family of two or more views is swapped (Square Enix's one-view submit is left alone). The swap is made
// just before the game's call and put back just after, checked against the pair it wrote, so the engine finds its
// family as it built it. Logged: the install, each change (Info), the first swapped family after a change (Debug), a
// put-back that could not be made (Warning, once). Reading: the switch and the families swapped since the last change.
// The renderer's copy follows the swap, so another test that takes the renderer's first view as player 1's (the
// lighting-volume test) moves to the other view while this one is on.
//
// Expected: haze moves to player 1's column -> it follows the place in the family (one family per player is the fix
// to build); it stays on player 2 -> it follows the player's view state or the column.

namespace lss::platform {
class GameImage;
}

namespace lss::render::view_order {

/// Verifies the renderer module's slot and replaces it (a pass-through while the switch is `stock`). False (logged)
/// otherwise.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::render::view_order
