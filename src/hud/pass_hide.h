#pragma once

// Each view's HUD image shows only its own player's world cursors (RE-hud-families.md §3, RE-field-icons-two.md; the
// user's first report, 2026-09-30: "player 1's blue talk/examine cursor shows in player 2's half").
//
// The turn-back alert (slot 36, over player 1's leader) is decided from player 1's leader and placed through player 1's
// view: player 1's alone. The field target icons (slot 15: talk, examine, chests, field actions) and the search compass
// and arrows (slot 55, world/search_arrows.h) are each player's own: each widget is placed through its own player's view.
// The claim screen's game windows (hud/view_window.h) are each view's own: each is shown in its seat's pass only, or a
// copy's in that copy's own pass (a copy of player 1's picture never shows player 1's window).
// Around each pass of the overlay render (hud/second_image.h) the roots that are not the pass's player's are made
// transparent through the game's own UUserWidget::SetColorAndOpacity, and their colour is put back right after, through
// the same setter: in a further column's pass player 1's cursors and the other seats' icons and search arrows; in player
// 1's pass (and the game's single render) the seats' icons and search arrows. They stay painted, so they are ticked as
// before. The further passes run first in the frame with DeltaTime 0: nothing the paint advances is lost, and the owner's
// pass paints it with its own colour. Game thread only.

namespace lss::hud::pass_hide {

/// Just before a further column's pass: what that column's seat (overlay_pass::SeatOf) does not see, transparent. A
/// mirror's column (no seat) shows player 1's HUD: the hides of player 1's pass, but the claim windows of its own view
/// (view_window::MirrorView). Returns how many were.
int BeginFurtherPass(int column) noexcept;
/// Just before player 1's pass or the single render: the seats' icons and windows transparent (nothing while no seat
/// has one).
int BeginOwnPass() noexcept;
/// Just after either: their colours back.
void EndPass() noexcept;

}  // namespace lss::hud::pass_hide
