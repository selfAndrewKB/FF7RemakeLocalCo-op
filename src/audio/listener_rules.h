#pragma once

// The sound watch's decisions as pure functions (audio/listener_watch.h), tested on the host
// (tests/listener_rules_test.cpp). The module keeps the threads, the engine reads and the redirects.

#include <array>
#include <cmath>

namespace lss::audio::listener_rules {

using Vec3 = std::array<float, 3>;

inline float Distance(const Vec3& a, const Vec3& b) noexcept
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

/// SEAD's audibility compare: a sound with a positive range is heard from a listener closer than that range (strictly,
/// as the game's `distance < range`).
constexpr bool Within(float range, float distance) noexcept
{
    return range > 0.0f && distance < range;
}

/// AutoSe's own conversion (SQEXSEADAutoSe_ConvertUeCmToMetres): UE cm (x, y, z) to metres (x, z, y). The watch
/// calls the game's converter; this is the host's copy, for the tests and as the fallback when its pointer is unset.
constexpr Vec3 UeCmToAutoSe(const Vec3& ue) noexcept
{
    return {ue[0] * 0.01f, ue[2] * 0.01f, ue[1] * 0.01f};
}

/// The clip's compare: beyond = |character - listener|^2 > distance^2 (both in AutoSe's space).
constexpr bool BeyondClip(const Vec3& character, const Vec3& listener, float distance) noexcept
{
    const float x = character[0] - listener[0], y = character[1] - listener[1], z = character[2] - listener[2];
    return x * x + y * y + z * z > distance * distance;
}

}  // namespace lss::audio::listener_rules
