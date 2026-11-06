#pragma once

// Each view's place in the engine's list, told by the view's own state rather than where it sits on screen (plan/topics/
// views.md: nothing may tie player 1's view to the first screen).
//
// The game viewport's family holds one view per local player in the list's order (player 1's, the seats', the
// mirrors'), each with the state its local player keeps for its whole life (FSceneView +0x08). At each of the game
// viewport's submissions (render/one_family, before any split) the game thread publishes those states by place; a
// render-thread reader matches a view's state to its place. Split frames already carry the place with each family
// (render/family_now); this is for a family of several views (`one-family off`, or the split unavailable), where the
// views sit at their screen places and only the top-left test told player 1's from the others before. A view whose
// state is not listed (a scene capture, a planar reflection) is no player's. Outside a split frame a join or leave may
// give one frame the previous list (the publish runs a frame ahead of the render; hud/ui_targets refuses a column taken
// twice in one Render); split frames read their place from render/family_now and need no guard for it.
//
// Written on the game thread, read on the render thread and its tasks.

#include <cstdint>

namespace lss::render::view_places {

/// PlaceOf's answer when nothing has been published (the split's slot is not installed): the caller keeps its own rule.
inline constexpr int kUnknown = -2;
/// PlaceOf's answer for a view no listed player's (a scene capture, a reflection).
inline constexpr int kNotListed = -1;

/// The game viewport's family (FSceneViewFamily) at its submission, before it is split. Game thread.
void Publish(const std::uint8_t* family) noexcept;

/// The place of `view` (an FSceneView or the renderer's FViewInfo) in the list last published, or kNotListed / kUnknown.
int PlaceOf(const void* view) noexcept;

}  // namespace lss::render::view_places
