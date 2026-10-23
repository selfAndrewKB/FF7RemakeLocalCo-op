#pragma once

// Shadows decided per view (RE-shadows-per-view.md, RE-shadows-per-view-build.md).
//
// Square Enix's light LOD takes some shadow decisions once for the whole view family, from one view or from all of
// them, so one player's camera changed the shadows in another player's view. Each lever makes one of them per view,
// the game's own decision taken with that view's data; each has a switch, so its frame cost can be measured:
//   - shadows-areas (A) and shadows-sun: each camera's evaluation pushes the light settings of the areas it stands in
//     (light, shadow and moving-shadow cutoffs, the sun's cascade distances) to the one scene, so every view used the
//     last camera's (player 3's: the user's report, Barret's shadows changing with Tifa's camera). The game's pushes are
//     tagged with their view on the game thread (in the data's padding), the render thread keeps each view's chain of
//     them (render/area_chains.h), and two stubs give each view its own set where the renderer reads it. No GPU cost of
//     its own; a view in a shorter-distance area draws fewer shadows, one in a longer area more, as its player alone.
//   - shadows-lamps (B): a lamp's shadow is no longer dropped for every view when one camera is beyond the lamp's
//     shadow cutoff; it is built when a view that sees the lamp is within the cutoff, and a view beyond it skips it.
//     Cost: lamps that a far camera removed today render their shadow (at most what each player alone would render).
//   - shadows-casters (C): a character casts a lamp's shadow when it is within the cutoff of the camera nearest to it,
//     not only of player 1's. Cost: the characters near players 2 and 3 are added to the lamps' shadows.
//   - fog-areas (A-fog): the same pushes carry each area's height fog, and the scene's first fog ended with the last
//     camera's. Each view's push is kept on the render thread (its setter writes every field, so a record is a
//     snapshot, not a chain; RE-fog-per-view-check.md) and a stub gives each view its own where InitFogConstants turns
//     the fog into that view's parameters. What the fog held before that loop is put back after it, so volumetric fog
//     (read later, from the first fog) stays the last camera's as in the game; and if anything but a push changed the
//     fog since (the inline setter of unthreaded rendering, a world-origin shift), the records are dropped.
// Every stub keeps the game's code path while its switch is off (the fog's put-back still runs, writing back the same
// bytes); with one view (single player) B and C do too, and A's stubs write the one view's own set, which is the
// game's.
// Render thread (the stubs) and the console (the switches: one byte each, read by the stubs at every use).

namespace lss::platform {
class GameImage;
}

namespace lss::render::shadows_per_view {

/// Checks the sites and installs the stubs (switches on). False (logged) if a site is not what the research read.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::render::shadows_per_view
