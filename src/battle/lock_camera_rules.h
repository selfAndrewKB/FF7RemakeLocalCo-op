#pragma once

// The lock camera readings' arithmetic, without the game (battle/lock_camera_two.h applies it).

#include <cmath>

namespace lss::battle::lock_camera_rules {

/// A yaw difference brought into [-180, 180]. The inputs are raw reads of the game's rotations, so an infinite or
/// absurd value must come back at once (it comes back as it is, or NaN), never loop.
inline float WrapDegrees(float degrees) noexcept
{
    return std::isfinite(degrees) ? std::remainder(degrees, 360.0f) : degrees;
}

}  // namespace lss::battle::lock_camera_rules
