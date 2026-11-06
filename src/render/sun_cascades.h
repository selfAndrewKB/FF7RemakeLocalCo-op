#pragma once

// Each view's own sun cascades where the game takes every view's (research/RE-haze-per-view.md; rule:
// render/sun_owner.h).
//
// With more than one view the game makes the sun's shadow cascades per view, each marked with its view, and two
// consumers then take the whole list without asking whose a cascade is (stock 4.18):
//   - each view's forward light data (ComputeLightGrid's cascade loop): every view's cascades are written into the
//     same four slots, so every view ends with the last view's cascades, and the cascade count adds up over the views.
//     Volumetric fog's sun shadowing and forward-lit see-through surfaces read it: players 1 and 2 used player 3's.
//   - the translucency lighting volume's light injection (four calls, each with the one volume's view): the sun is
//     injected once per view's cascade set, so the volume gets the sun two or three times, shadowed by other cameras'
//     cascades.
// The game's own rule elsewhere is that a cascade with a view is that view's. So the fix keeps that rule at these two
// consumers, through the game's own calls: the cascade test in the forward light loop (its one call to
// IsWholeSceneDirectionalShadow) answers no for another view's cascade, and each injection call gets the game's list
// without another view's shadowed entries. One view: nothing differs (every cascade is its own). Render thread.
// Switch `sun-cascades` (on); reading `sun-cascades` (cascades turned away, by consumer).

namespace lss::platform {
class GameImage;
}

namespace lss::render::sun_cascades {

/// Verifies the targets and the five call sites and redirects them. False (logged) otherwise; nothing changed then.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::render::sun_cascades
