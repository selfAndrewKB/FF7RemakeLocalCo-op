#pragma once

// Which player's family the renderer is drawing now, while one family per player is drawn (render/one_family.h).
//
// The game thread submits a frame's families one after another; the render thread draws them in that order. Before
// each family the split queues a render command (render/family_boundary.h) that sets this to {k, n, state}: the family
// holds view k of the game's n (the player's column, or a mirror's), whose view state is `state`; after the last family
// one that sets it back to none. Everything the renderer runs inside that family's submission reads the same value,
// including work that is not the player's view (planar reflections and deferred captures drawn from the submission), so
// readers that act per view take it through For(view), which keeps it only for the player's own view (by its state).
// Outside a split frame (one view, the split off, the menu's own viewport) it is none, and the readers keep their rules
// for one family of several views.
//
// Written on the render thread (by the queued command), and cleared from any thread when the boundary faults; read on
// the render thread and the tasks it waits on.

#include <cstdint>

namespace lss::render::family_now {

struct Family
{
    std::int32_t index = -1;       // the view's place in the game's family; -1: not a split frame
    std::int32_t count = 0;        // the views in the game's family
    const void*  state = nullptr;  // that view's state (FSceneView+0x08)
};

/// The family being drawn, whatever view asks.
Family Current() noexcept;

/// The family being drawn if `view` (an FSceneView or the renderer's FViewInfo) is its player's view, else none.
Family For(const void* view) noexcept;

/// The index as a plain 32-bit value, for machine code (render/shadow_code.h): -1 outside a split frame. The stubs check
/// the view's state against their record themselves.
const std::int32_t* IndexAddress() noexcept;

/// The queued command (render thread, or the game thread when rendering is not threaded); {} after a fault, any thread.
void Set(Family family) noexcept;

}  // namespace lss::render::family_now
