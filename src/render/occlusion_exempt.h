#pragma once

// A test for the blue haze in player 2's and player 3's views (RE-haze-occlusion.md): chosen objects skip their
// occlusion test and count as visible, in all views or only some (the switch below). Off until the command sets
// objects.
//
// Clearing every object's "can be occluded" flag (FScene::PrimitiveOcclusionFlags bit 0) from outside the game cures
// the haze, and halving the set narrowed it, but the game rewrites some objects' flags every frame (animated
// background layers, emissive parts, enemies), so a hold from outside covers them only part of the time [CONFIRMED
// live 2026-10-23 23:48]. This test applies the set where the flags are read: a redirect on the one call to
// FetchVisibilityForPrimitives_Range (each view's occlusion answers, run for one view at a time on the render thread)
// clears bit 0 for the set's objects, calls the game's function, and puts each cleared bit back. An object without the
// bit is never tested and counts as visible; nothing else reads the change, and no gap remains.
//
// Command `occlusion-exempt <a b, c d>`: object indices in the scene (from a, up to but not including b), or `none`.
// The indices are the main scene's (the one with the most objects); only its passes are reported. Logged: the install
// and each new set (Info, with the objects' names when there are few), a change in how many objects a pass exempts
// (Trace: the game rewrites some flags every frame). The reading gives the set and the last count in an allowed view.
//
// Switch `occlusion-exempt.views all | player-1 | others`: the views the set applies in (player 1's = the first view of
// the engine's list, told by its state: render/view_places, wherever it sits on screen; a scene capture's view is no
// player's and counts with the others; with no list known, the view whose rectangle starts at the window's top-left
// corner). Exempting in player 1's view only tells a limit shared by all views' queries from a fault in the
// answers of player 2's and player 3's own views (2026-10-24: the latter).
//
// Command `occlusion-exempt.third-column <a b, c d> | none | main`: a set of its own for the third column's view (the third
// view of the engine's list; with no list known, the view whose rectangle's left edge is two widths in, in the top row,
// which is another player's view under the column order `1 3 2`; it then ignores `.views`), so one look tests two
// sets: player 2's view takes the main set (with `.views others`), player 3's its own. `main` gives it the main set
// again. Halving with the depth-pyramid test on (`r.HZBOcclusion 1`, which tests each object alone, not in groups
// of 8) gives clear answers.

namespace lss::platform {
class GameImage;
}

namespace lss::render::occlusion_exempt {

/// Verifies the function and its call site and redirects the call (a pass-through until a set is given). False
/// (logged) otherwise.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::render::occlusion_exempt
