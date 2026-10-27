#pragma once

// The lock camera readings' arithmetic, without the game (battle/lock_camera.h applies it).

#include <cmath>
#include <cstdint>

namespace lss::battle::lock_camera_rules {

/// A yaw difference brought into [-180, 180]. The inputs are raw reads of the game's rotations, so an infinite or
/// absurd value must come back at once (it comes back as it is, or NaN), never loop.
inline float WrapDegrees(float degrees) noexcept
{
    return std::isfinite(degrees) ? std::remainder(degrees, 360.0f) : degrees;
}

/// A camera jump: one tick that turned it more than 10 degrees at over 720 degrees a second, or moved it more than
/// a metre at over 25 metres a second. A stick turn, a follow or a dodge stays under both; a cut, a reset or another
/// writer does not. Without a usable tick time (0, negative, not finite) the step sizes alone decide. A step that
/// is not finite (a bad read) is no jump.
inline bool IsJump(float turnDegrees, float moveCm, float deltaSeconds) noexcept
{
    constexpr float kTurn = 10.0f, kTurnRate = 720.0f, kMove = 100.0f, kMoveRate = 2500.0f;
    const bool  timed = std::isfinite(deltaSeconds) && deltaSeconds > 0.0f;
    const bool  turn  = std::isfinite(turnDegrees) && turnDegrees > kTurn && (!timed || turnDegrees / deltaSeconds > kTurnRate);
    const bool  move  = std::isfinite(moveCm) && moveCm > kMove && (!timed || moveCm / deltaSeconds > kMoveRate);
    return turn || move;
}

/// A burst of jumps opens on a jump and stays open until this many frames pass without one (a fifth of a second at
/// 120 fps), so a camera that jumps every few ticks is one burst (one line at its start, one at its end), not a line
/// per jump, while jumps a visible moment apart each get their own line.
inline constexpr std::uint64_t kQuietFrames = 24;

/// Whether a burst whose last jump was at engine frame `lastJump` has closed by `frame` (a frame counter that went
/// back closes it too).
constexpr bool BurstClosed(std::uint64_t lastJump, std::uint64_t frame) noexcept
{
    return frame < lastJump || frame - lastJump > kQuietFrames;
}

}  // namespace lss::battle::lock_camera_rules
