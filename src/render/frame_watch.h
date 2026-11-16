#pragma once

// How long each frame takes, and which thread takes it, for performance work (research/RE-frame-stats.md).
//
// Once per frame, first in the engine tick on the game thread: the frame's time (the performance counter between two
// ticks) against a running average (platform/hitch_step.h: a hitch is well over it), and the engine's own numbers for
// the last frame, the ones `stat unit` shows: the game thread's work, the render thread's work and its present, and the
// GPU's frame. At the end of each window (10 s; `frame-watch.window` sets 1 to 10 s, short enough to bracket an attack
// of a few seconds) one Debug line gives the frames and the frame rate, the average and worst frame, the hitches, each
// engine number's average and worst, and each frame thread's CPU time per frame as Windows counts it (the game, render
// and RHI threads; the engine keeps no number for the RHI thread). A thread whose CPU time is near the frame's time
// binds; with none near it, the GPU does. A GPU number unchanged through a window is stale (the engine's recorder of
// it is off) and is named so. Each hitch gets a Debug line with its time and the engine's numbers. `frame-watch` reads
// the last window's line. Logging only: nothing of the game's is written.

namespace lss::platform {
class GameImage;
}

namespace lss::render::frame_watch {

/// Checks the engine's frame-time globals and the frame threads' globals against the research. The frame's own time is
/// watched either way; a group whose check fails is left out of the lines (logged). True.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, first in each engine tick: ends the last frame, logs a hitch, and a window's line at its end.
void OnFrameStart() noexcept;

}  // namespace lss::render::frame_watch
