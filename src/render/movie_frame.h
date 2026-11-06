#pragma once

// The pre-rendered movie in every view (research/RE-movie-view.md, pass S-RMK-MOVIEVIEW, reviewed in IDA).
//
// A movie is not a widget and not a material: the media texture's frame is stored once per frame in the renderer's
// composite source (FSceneRenderTargets+0x120), the screen filter's image-effects component raises the composite
// intensity to 1 in every view's post-process settings (a world volume), and each view's final tonemap pass draws
// that texture over its own rectangle. FPostProcessing::Process ends with an unconditional release of the source,
// once per view, so the first view consumes the frame and the second view's final pass binds the black fallback:
// the mirror player's column was black through the opening (seen live). Square Enix guards its other
// per-family target in the same function with a last-view test (0x14251DCCE); the composite source did not get it.
// The one call is redirected (platform/call_redirect.h, the five bytes inside one block) and runs the game's own
// release only for the family's last view, which hud/ui_targets' post-process wrapper names (it sees every view's
// order in the Render). Nothing else changes: the lifetime is single player's, one release per frame, after the
// last composite. Render thread. Switch `movie-frame on|off` (on), reading `movie-frame.state`.

namespace lss::platform {
class GameImage;
}

namespace lss::render::movie_frame {

/// Checks the call and the release function's bytes and redirects the call. False (logged) if they differ; the
/// second view then keeps the black movie column.
bool Install(const platform::GameImage& image) noexcept;
/// Puts the call back (unload, with the split off).
void Remove() noexcept;

/// Render thread, from the post-process wrapper: the view about to be post-processed is the family's last (or the
/// order is unknown, which releases as the game does), and its end.
void BeginView(bool last) noexcept;
void EndView() noexcept;

void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

}  // namespace lss::render::movie_frame
