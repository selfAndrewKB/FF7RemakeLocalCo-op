#pragma once

// Square Enix's AO downsample, stage 0, made to read each view's own rows (the split-screen ghost).
//
// The stage-0 pixel shader loads the full-resolution AO trace at `tap + 2*SV_Position`, and its tap offsets are
// the only position it gets: the view rectangle's origin is never added. Player 1's view starts at (0,0), so it is
// correct; player 2's view (origin (0,540) top/bottom, (960,0) side by side) reads player 1's rows, and the game's
// later AO passes (which do handle the origin) blend player 1's shading into player 2's picture: a faint copy of
// player 1's view in player 2's half. Research: RE-rdc-split-frame.md (RenderDoc proof), RE-impl-facts-ao-fix.md.
//
// No game path exists (an engine bug with no setting), so this is a documented minimum fix: the stage-0 call of
// the shared SetParameters is redirected to note its view for the length of the call (thread-local), and the
// function's two tap-value calls are redirected to add that view's rectangle origin to both taps of each value.
// Stages 1 and 2, which share the function but ignore the taps, are untouched. With the origin at (0,0) (solo, or
// player 1) the values are passed through unchanged.

namespace lss::platform {
class GameImage;
}

namespace lss::render::ao_origin {

/// Verifies the three call sites and their targets and redirects them. False (logged) if anything differs.
bool Install(const platform::GameImage& image) noexcept;

/// True once installed.
bool Installed() noexcept;

/// On by default; off passes every value through (for A/B checks). Any thread.
void SetEnabled(bool enabled) noexcept;
bool Enabled() noexcept;

}  // namespace lss::render::ao_origin
