#pragma once

// Between two players' view families, the game's own end-of-frame state reset, in RHI stream order; and, before each
// family, which player's family the renderer draws next (render/family_now.h). RE-haze-occlusion.md Pass 10 I.1;
// ported from Rebirth's renderer/family_boundary (its crash S-CRASH2), with this build's 4.18 layout.
//
// Why: the D3D12 state cache keeps raw, uncounted SRV pointers in fixed slots, and only
// FD3D12CommandContext::ClearState empties it, once a frame in RHIEndFrame. Every scene render flushes pending
// deletions twice, and a released resource dies on the fourth flush after the one that took it, so with three families
// a view bound in family 1 can be destroyed in family 3 of the same frame while the cache still names it; a later draw
// whose shader covers that slot then reads freed memory. With one family a frame the end-of-frame clear always comes
// first.
//
// What: before family k+1 the default direct context runs exactly what RHIEndFrame runs between frames: ClearState,
// then the context's submit (ClearState alone is unsafe mid-list: it resets cached values without their re-send flags;
// the submit's reopen re-flags them, as RHIEndFrame pairs them). Each family then starts from the state the first family
// of a frame starts from.
//
// How, all through the game's own functions and objects:
//   1. game thread, before each family's submission: a render-thread task queued as BeginRenderingViewFamily queues
//      its own draw command (the task graph's QueueTask, vt[0] of GTaskGraph, to ENamedThreads::RenderThread), so it
//      runs after the previous family's commands and before this family's; while rendering is not threaded
//      (GIsThreadedRendering 0), its body runs at once, as the game's own test does;
//   2. render thread: the task records which family comes next (render/family_now.h) and, before every family after
//      the first, appends a 16-byte node to the immediate RHI command list exactly as the game appends its own submit
//      node (or runs the reset at once while the list is walked in place, the game's rule without an RHI thread);
//   3. RHI thread, in stream order: the game's ClearState and submit on the list's context, only if it is the default
//      direct context.
// With GPU view marks on (render/view_marks.h, a dev switch) the render thread also appends a node before the first
// family and after the last; every node then submits and marks the view on the context's queue. The node holds no data,
// so the view's index is the node's function (one instance per index).
// The exception to "only the game's code": the task's two-entry vtable, the node's function and the node append, which
// the game inlines into every command and never exposes as a function. There is no game command that clears state
// mid-frame; this is the least that issues one (Rebirth recorded the same exception).

#include "render/family_now.h"

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::render::family_boundary {

/// Verifies every function, slot and global it uses and pins the DLL (queued work may outlive an unload). False (logged),
/// changing nothing, otherwise; one family per player then stays off.
bool Install(const platform::GameImage& image) noexcept;

/// Installed and not faulted: families may be split.
bool Ready() noexcept;

/// A byte the render thread writes in the boundary task, before the family's commands, in stream order (the motion
/// table's paused byte, render/motion_history.h). Written under a guard; a fault there is counted and leaves the boundary on.
struct ByteWrite
{
    std::uint8_t* at    = nullptr;  // null: nothing to write
    std::uint8_t  value = 0;
};

/// Game thread, before a family's submission (or after the last, with index -1): the next family the render thread
/// draws, whether the boundary reset runs before it (every family after the frame's first) and a byte to write before
/// it. False if it could not be queued (not ready, or a fault now): the family must not be drawn as a family of its own.
bool Queue(family_now::Family next, bool reset, ByteWrite write = {}) noexcept;

/// Game thread: a fault once (Warning), and every 30 s while families are split, what the boundary did (Debug).
void Poll() noexcept;

/// The reading's part: what the boundary did since the start. May throw (out of memory).
std::wstring Describe();

}  // namespace lss::render::family_boundary
