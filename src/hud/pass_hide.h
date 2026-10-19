#pragma once

// Player 1's own world cursors out of player 2's HUD image (RE-hud-families.md §3, field target icons and search
// arrows; the user's first report, 2026-09-30: "player 1's blue talk/examine cursor shows in player 2's half").
//
// The field target icons (slot 15: talk, examine, field actions) and the search arrows (slot 55) are decided from player
// 1's leader and placed through player 1's view; no per-player storage exists, so player 2 gets none of their own yet
// (Rebirth kept talk/examine player 1's, the user's call there). Around player 2's pass of the overlay render
// (hud/second_image.h) each such root on the overlay is made transparent through the game's own
// UUserWidget::SetColorAndOpacity, and its colour is put back right after, through the same setter. It stays painted,
// so it is ticked as before. Player 2's pass runs first in the frame with DeltaTime 0: nothing the paint advances is
// lost, and player 1's pass paints it with its own colour. Game thread only.

namespace lss::hud::pass_hide {

/// Just before player 2's pass: player 1's cursors transparent. Returns how many were.
int BeginPlayerTwoPass() noexcept;
/// Just after it: their colours back.
void EndPlayerTwoPass() noexcept;

}  // namespace lss::hud::pass_hide
