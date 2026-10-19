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
//   H1  AllocBind (0x1421E82E9; the clear-only path 0x1421E826B): for player 2's view the member is moved out first
//       (the reference moves with it), so the game's own allocator hands player 2's UI pass a second target from its
//       own pool, with its own descriptor;
//   H4  FPostProcessing::Process (0x1421E8400): the member names the view's own target while that view's final pass
//       sets its parameters; the other one stays held. After the last view the mod releases what it holds, so it owns
//       no reference across Renders (the game's own lifetime: one Render).
// Player 2's view is the one whose ViewRect.Min is not (0, 0) (both split shapes). With one view nothing moves.
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

/// True once the render thread has given player 2's view its own target in a Render since the last call (game thread).
bool TakeSecondTargetSeen() noexcept;

}  // namespace lss::hud::ui_targets
