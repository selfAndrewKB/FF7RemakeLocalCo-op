#pragma once

// Every frame around a heavy moment, kept in a file without anyone at the console (performance work;
// research/RE-frame-stats.md).
//
// The frame watch (render/frame_watch.h) hands each frame's numbers here: its time, the engine's game, render, present
// and GPU times, each frame thread's CPU time and the mod's own engine-tick callback (the one that ran within the frame,
// the frame watch's own work and a capture's file write included). The last frames are kept in a
// ring. A capture starts (render/capture_rules.h) when a watched enemy action starts (`frame-capture.actions`, the
// Airbuster's beams at start: the enemy watch's gate feed, battle/enemy_watch.h, runs for it even without Debug lines),
// or when, in battle, the frame time stays well over its baseline (`frame-capture.drops`). It takes the 5 s before the
// start from the ring, runs while the trigger holds, keeps 3 s after (60 s at most), then writes one CSV file per
// capture to `captures\` beside the settings file (one row per frame, each marked before, during or after; its head
// names the trigger, the actions seen and the views) and one Info line comparing before, during and after: the frame
// rate, the average, 95th-percentile and worst frame, each engine number, each thread's CPU time and the mod's tick.
// The views (1, 2 or 3) are in both, so a solo run and a split run of the same attack compare directly. The file is
// written on the game thread when the capture ends: a hitch there is the writer's. At most 30 captures a launch.
// Logging only: nothing of the game's is written.

#include <array>
#include <cstddef>

namespace lss::platform {
class GameImage;
}

namespace lss::render::frame_capture {

/// The engine's numbers, as `stat unit` shows them.
enum EngineNumber : std::size_t
{
    kGameTime,
    kRenderTime,
    kPresentTime,
    kGpuTime,
    kEngineNumbers,
};

/// The frame threads.
enum FrameThread : std::size_t
{
    kGameThread,
    kRenderThread,
    kRhiThread,
    kFrameThreads,
};

/// One frame's numbers, in ms; a negative one is unread (a check failed, or the thread is not followed yet).
struct Sample
{
    double                                 frameMs = 0.0;
    std::array<double, kEngineNumbers>     engineMs{-1.0, -1.0, -1.0, -1.0};
    std::array<double, kFrameThreads>      cpuMs{-1.0, -1.0, -1.0};
    double                                 modMs = -1.0;  // the mod's engine-tick callback
};

/// Registers the settings and asks the enemy watch for its actions. True.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, once per frame (the frame watch): keeps the frame, and starts, runs or ends a capture.
void Add(const Sample& sample) noexcept;

}  // namespace lss::render::frame_capture
