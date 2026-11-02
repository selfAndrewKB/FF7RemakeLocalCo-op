#pragma once

// GPU events around each player's view family, for GPU profilers (Nsight Graphics' GPU Trace, PIX): "LSS view 1",
// "LSS view 2", ... on the D3D12 direct queue, so a trace splits the frame's GPU time by player view. The shipping
// build compiles out the engine's own draw events, so without these a trace has no names at all
// (research/RE-perf-3p-2026-11-02.md §5a).
//
// A dev switch, off at start (`view-marks on|off`): marking submits the open command list before the first family and
// after the last (the reset already submits between families), so it costs a little GPU idle while on. It is turned on
// for a trace only (tools/perf/gputrace.ps1).
//
// How: the family boundary (render/family_boundary.h) already reaches the RHI thread in stream order before every
// later family. With marks on it also does so before the first family and after the last; there, on the default
// direct context and after its submit (the game's RHISubmitCommandsHint), Mark ends the open view's event and begins
// the next one on the context's command queue. Queue events, not command-list events: a view's commands span several
// command lists (the game submits mid-family), and the submit just before each mark makes the queue position the
// exact boundary. The event format is PIX's plain one (metadata 0, a wide string), which every tool reads.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::render::view_marks {

/// Verifies the instructions behind the context-to-queue path and registers the switch. False (logged) otherwise: the
/// switch is then absent and nothing is marked.
bool Install(const platform::GameImage& image) noexcept;

/// Any thread: installed and switched on.
bool On() noexcept;

/// Any thread: an event (a view's or a pass's) is still open on the queue (a marked frame that stopped before its end
/// mark). The family boundary then closes it before the next frame's first family, so a trace never nests later frames
/// inside it.
bool Open() noexcept;

/// RHI thread (the render thread without one), in stream order, on the default direct context right after its
/// submit: ends every open event (a pass left open, then the view's), then begins view `index`'s (index -1, after the
/// last family: only ends).
void Mark(void* context, int index) noexcept;

/// The same thread and point: begins a pass's event (render/pass_marks.h), nested in the open view's if there is one.
void BeginPass(void* context, const wchar_t* name) noexcept;

/// The same thread and point: ends the innermost pass event, if one is open (never a view's).
void EndPass() noexcept;

}  // namespace lss::render::view_marks
