#pragma once

// When the frame capture (render/frame_capture.h) starts and ends, as a pure step, tested on the host
// (tests/capture_rules_test.cpp).
//
// A capture starts on the first frame of a trigger: a watched enemy action running, or, in battle, the frame time
// staying well over its baseline for a while (a frame-rate drop). It runs while either holds, keeps the frames for a
// while after, and ends there or at its longest. The baseline is a slow average of the frames outside captures; the
// drop is read on a fast average. Both count each frame at most twice the baseline, so a single long frame (a hitch, a
// stall, the capture's own file write) neither moves the baseline nor reads as a drop.

namespace lss::render::capture_rules {

struct Config
{
    double beforeMs     = 5000.0;   // the frames kept from before the start
    double afterMs      = 3000.0;   // the frames kept after the trigger ends
    double longestMs    = 60000.0;  // a capture ends at this length whatever still runs
    double dropRatio    = 1.2;      // a drop: the fast average over the baseline by this ratio...
    double dropMarginMs = 1.5;      // ...and by this many ms...
    double dropHoldMs   = 500.0;    // ...for this long (of frames counted as at most twice the baseline)
    double baselineTauMs = 5000.0;  // the baseline's time constant
    double recentTauMs   = 250.0;   // the fast average's
};

enum class Phase
{
    Idle,
    During,  // a trigger holds
    After,   // no trigger holds: the frames after it
};

enum class Reason
{
    None,
    Action,  // a watched enemy action
    Drop,    // a frame-rate drop in battle
};

struct State
{
    Phase  phase      = Phase::Idle;
    Reason reason     = Reason::None;  // what started the capture running or last ended
    double baselineMs = 0.0;           // 0: no frame yet
    double recentMs   = 0.0;
    double dropForMs  = 0.0;  // how long the drop has held
    double lengthMs   = 0.0;  // the capture's length so far (from its start, the frames before it not counted)
    double afterForMs = 0.0;  // how long the trigger has not held
};

struct Frame
{
    double frameMs = 0.0;
    bool   action  = false;  // a watched enemy action runs
    bool   battle  = false;  // in battle (the drop trigger runs only there)
    bool   drops   = true;   // the drop trigger is on
};

enum class Step
{
    Idle,     // no capture
    Start,    // this frame starts one (the frames before it are taken first)
    Capture,  // this frame belongs to the running one (State::phase says During or After)
    End,      // this frame is the running one's last
};

/// One average step toward `value` with time constant `tauMs` over a frame of `frameMs`.
constexpr double Toward(double average, double value, double frameMs, double tauMs) noexcept
{
    return average + (value - average) * (frameMs / (tauMs + frameMs));
}

/// Whether the fast average is well over the baseline (the drop's condition, before its hold).
constexpr bool Dropping(const State& s, const Config& c) noexcept
{
    return s.baselineMs > 0.0 && s.recentMs > s.baselineMs * c.dropRatio && s.recentMs > s.baselineMs + c.dropMarginMs;
}

constexpr Step CaptureStep(State& s, const Config& c, const Frame& f) noexcept
{
    if (f.frameMs <= 0.0)
    {
        return s.phase == Phase::Idle ? Step::Idle : Step::Capture;
    }
    if (s.baselineMs == 0.0)
    {
        s.baselineMs = s.recentMs = f.frameMs;
        return Step::Idle;
    }
    // Each frame counted at most twice the baseline, in its value and its length, by the fast average and the drop's
    // hold: one long frame (a hitch, a stall) is not a drop.
    const double counted = f.frameMs < s.baselineMs * 2.0 ? f.frameMs : s.baselineMs * 2.0;
    s.recentMs           = Toward(s.recentMs, counted, counted, c.recentTauMs);
    const bool dropping = f.drops && f.battle && Dropping(s, c);
    s.dropForMs         = dropping ? s.dropForMs + counted : 0.0;
    const bool drop     = s.dropForMs >= c.dropHoldMs;

    if (s.phase == Phase::Idle)
    {
        if (f.action || drop)
        {
            s.phase      = Phase::During;
            s.reason     = f.action ? Reason::Action : Reason::Drop;
            s.lengthMs   = f.frameMs;
            s.afterForMs = 0.0;
            return Step::Start;
        }
        s.baselineMs = Toward(s.baselineMs, counted, f.frameMs, c.baselineTauMs);
        return Step::Idle;
    }

    s.lengthMs += f.frameMs;
    // While capturing, the drop holds on its condition alone: its hold was met at the start, or the action started it.
    const bool holds = f.action || dropping;
    if (holds)
    {
        s.phase      = Phase::During;
        s.afterForMs = 0.0;
    }
    else
    {
        s.phase = Phase::After;
        s.afterForMs += f.frameMs;
    }
    if (s.lengthMs >= c.longestMs)
    {
        // Ended at its longest: a drop still holding is the new normal, so the next capture needs a new one.
        s.baselineMs = dropping ? s.recentMs : s.baselineMs;
        s.phase      = Phase::Idle;
        s.dropForMs  = 0.0;
        return Step::End;
    }
    if (s.phase == Phase::After && s.afterForMs >= c.afterMs)
    {
        s.phase     = Phase::Idle;
        s.dropForMs = 0.0;
        return Step::End;
    }
    return Step::Capture;
}

}  // namespace lss::render::capture_rules
