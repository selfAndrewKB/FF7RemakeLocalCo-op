#pragma once

// One UI image per view, on the render thread (RE-hud-two-images.md §4, §5, §7: H1 and H4).
//
// The game draws the HUD's UI image once per view, inside FDeferredShadingSceneRenderer::Render's UI loop, into the one
// pooled target FSceneRenderTargets+0x110 (allocated from the pool by SceneRenderTargets__AllocBindInGameUITarget
// while the member is null, released by the game at every Render start), and each view's final pass
// (SE_RCPassFinalTonemapUI, inside FPostProcessing::Process) samples whatever the member names when its parameters are
// set. With two views the loop draws both images before either is post-processed, so the last one wins in both halves.
//
// Two calls in Render are redirected (platform/call_redirect.h), both with the view in r8:
//   H1  AllocBind (0x1421E82E9; the clear-only path 0x1421E826B): for a further column's view the member is moved
//       out first (the reference moves with it), so the game's own allocator hands that UI pass a target of its own
//       from its own pool, with its own descriptor;
//   H4  FPostProcessing::Process (0x1421E8400): the member names the target the view samples while that view's final
//       pass sets its parameters (its own column's; column 0's for a mirror's column, which has no HUD image of its
//       own); the others stay held. After the last view the mod releases what it holds, so it owns no reference
//       across Renders (the game's own lifetime: one Render).
// The views are numbered by column (hud/overlay_pass.h): outside a split frame a view's column is its place in the
// engine's list, told by its state (render/view_places), so player 1's view is column 0 wherever it sits on screen; in a
// split frame each family's one view is at (0, 0) and the family's place decides the image (render/family_now). With no
// list known the view at (0, 0) is column 0 and the next ones are counted in the UI loop's order. With one view nothing
// moves. The
// bookkeeping is a pure rule, hud/target_rotation.h, host-tested for one to three columns (plan/seats.md step 5).
// The switch `hud-targets` (off at start until the image content follows) turns the moves on; the logging runs either
// way (`hud-targets-log <frames>` logs each call for that many frames).

namespace lss::platform {
class GameImage;
}

namespace lss::hud::ui_targets {

bool Install(const platform::GameImage& image) noexcept;
/// Puts the calls back. At unload, with the split off.
void Remove() noexcept;

/// The `hud-targets` switch: each view samples its own UI target.
void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

/// Game thread, each tick: the columns with a HUD image of their own (player 1's, the seats' and, on monitors of
/// another shape, the mirrors'; 1..kMaxColumns); the columns beyond are the mirrors', whose views sample column 0's image.
void SetPlayerColumns(int columns) noexcept;

}  // namespace lss::hud::ui_targets
