#pragma once

// Player 1's own world cursors out of the further columns' HUD images (RE-hud-families.md §3, field target icons and
// search arrows; the user's first report, 2026-09-30: "player 1's blue talk/examine cursor shows in player 2's half").
//
// The field target icons (slot 15: talk, examine, field actions), the search arrows (slot 55) and the turn-back alert
// (slot 36, over player 1's leader) are decided from player 1's leader and placed through player 1's view; no
// per-player storage exists, so the further players get none of their own yet (Rebirth kept talk/examine player 1's,
// the user's call there). Around a further column's pass of the overlay render (hud/second_image.h) each such root on
// the overlay is made transparent through the game's own UUserWidget::SetColorAndOpacity, and its colour is put back
// right after, through the same setter. It stays painted, so it is ticked as before. The further passes run first in
// the frame with DeltaTime 0: nothing the paint advances is lost, and player 1's pass paints it with its own colour.
// Game thread only.

namespace lss::hud::pass_hide {

/// Just before a further column's pass: player 1's cursors transparent. Returns how many were.
int BeginFurtherPass() noexcept;
/// Just after it: their colours back.
void EndFurtherPass() noexcept;

}  // namespace lss::hud::pass_hide
