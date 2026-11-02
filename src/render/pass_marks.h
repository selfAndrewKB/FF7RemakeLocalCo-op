#pragma once

// GPU events around each render pass of a view (PrePass, Shadow depths, Base pass, Lights, Translucency,
// Post-processing, the Square Enix passes, ...), nested in the view's event (render/view_marks.h), so a GPU trace
// splits each player's view by pass. The shipping build has no draw-event names of its own
// (research/RE-perf-3p-2026-11-02.md).
//
// A dev switch, off at start (`pass-marks on|off`). While on it costs: two submits per pass (about 100 a view), which
// leave the GPU idle between passes; a pass's own time stays exact (its commands sit between its two marks), the
// view's total grows. It is turned on for a trace only (tools/perf/gputrace.ps1).
//
// How: each pass is one direct call in the scene renderer (FDeferredShadingSceneRenderer::Render 0x1421E64A0; 59
// sites, research/.ida-mcp/pass-marks/_sites.tsv, byte-checked at install). While on, each call goes to a wrapper
// (platform/call_redirect.h) that puts a node on the RHI command list before and after the original call; on the RHI
// thread each node submits and begins or ends the pass's event on the queue (family_boundary::SubmitForMark,
// view_marks::BeginPass / EndPass). Every site takes four integer or pointer arguments at most and returns in rax, so
// one wrapper shape fits all. The GPU particles have no direct call (the effects system's vtable): their event runs
// from the return of the call before them to the call after them, closed by the next pass if that call does not run.
// 19 of the calls cross a 16-byte block, so every redirect is placed and removed on the thread that runs the renderer,
// outside it: the render thread's family-boundary task before a split frame's first family (BetweenFamilies). So the
// switch takes effect at the next split frame only: switched off while one player is alone, the redirects stay until
// players split again, and an unloaded mod leaves them (safe: the DLL is pinned, and with the boundary gone the
// wrappers mark nothing and just call the game).

namespace lss::platform {
class GameImage;
}

namespace lss::render::pass_marks {

/// Checks every site's bytes and registers the switch. False (logged), changing nothing, if any differs.
bool Install(const platform::GameImage& image) noexcept;

/// The render thread (the thread that runs the scene renderer), from the family boundary's task, outside the renderer:
/// places or removes the redirects as the switch asks (before a frame's first family), and drops a particles event
/// left open by the family before.
void BetweenFamilies(bool firstFamily) noexcept;

}  // namespace lss::render::pass_marks
