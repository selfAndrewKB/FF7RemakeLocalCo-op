#pragma once

// The translucency lighting volume with more than one view (RE-haze-per-view.md §2, §5, §9): the blue haze in players
// 2's and 3's views.
//
// The volume is a small 3-D grid of light that see-through surfaces (mist sheets, light cards, particles, Square
// Enix's scattering modes) take their light from. Stock 4.18 keeps one per frame and fills it for Views[0] only, over
// a box ahead of that view's camera; every view then samples it with its own box (each view's uniforms). Players 2 and
// 3 read player 1's light at the wrong places, and the box follows player 1's camera direction and snaps to voxel and
// field-of-view steps, so their haze changes and flickers as player 1 turns. [CONFIRMED code; that it is the haze:
// ASSUMPTION, this switch tests it]
//
// For now this holds the discriminating test only: switch `volume-box` (off) gives the further views player 1's box,
// the game's own rule for a stereo pair (the right eye takes the left eye's box, 0x1425C4F55..F6A), through the one call
// that computes a view's box. They then read player 1's grid at the right places: where player 1's box reaches, their
// mist should look as player 1's does. Not the fix (outside player 1's box they read its edge); the fix is each view's
// own grid, filled by the game's own steps (§5). Reading `volume-box.views` (view setups that took player 1's box).

namespace lss::platform {
class GameImage;
}

namespace lss::render::lighting_volume {

/// Verifies the target and its call site and redirects it (a pass-through while the switch is off). False (logged)
/// otherwise; nothing changed then.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::render::lighting_volume
