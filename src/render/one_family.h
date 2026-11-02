#pragma once

// One view family per player: the game viewport's frame is handed to the renderer one player at a time, each player the
// first and only view of its own family (render/family_split.h; RE-haze-occlusion.md Pass 9, Pass 10;
// research/RE-one-family-ledger.md for the parts of the mod that follow the family).
//
// Why: in one family of several views, every view after the first loses its volumetric fog after a late join (the
// flickering blue haze), and the haze follows the place in the family, not the player [CONFIRMED live 2026-10-24 07:20,
// the view-order swap]. Each player first in its own family is drawn as the game draws a single player: its own
// occlusion pass, light grid, fog, translucency lighting volume, sun cascades and post-processing (Rebirth renders its
// players the same way).
//
// How: the renderer module's BeginRenderingViewFamily slot is replaced; only the game viewport's call (UGameViewportClient
// ::Draw, by its return address) with two or more views is split, and only while the family boundary is ready (its
// reset keeps the D3D12 state cache safe across families: render/family_boundary.h). Before each family the boundary
// queues the family's place for the render thread (render/family_now.h), which the HUD image, the movie frame and the
// area settings read; after the last family, none. Every other submission (the menu's viewport, one player) goes to the
// game unchanged. Every game viewport submission, split or not, first publishes each view's place in the list by its
// state (render/view_places), for the readers outside a split frame. The final pass loads the back buffer and draws into the view's unshifted rectangle, so each family keeps
// the columns before it [CONFIRMED code, Pass 10 I.2]; GPU particles step once a frame (the later families simulate with
// zero time, Pass 10 I.3); the motion table steps with the first family only (render/motion_history.h).
//
// Switch `one-family on | off` (on at start): off hands the game its own family of several views, for an A/B. The
// reading gives the switch, the frames split, the families of the last split frame and what the boundary did.

namespace lss::platform {
class GameImage;
}

namespace lss::render::one_family {

/// Replaces the slot (the family boundary must be installed first). False (logged) otherwise: one family of several views.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::render::one_family
