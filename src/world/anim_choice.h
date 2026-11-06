#pragma once

// Which camera the game's animation judge reads for one mesh, and a further player's camera snapshot in the game's own form
// (research/RE-anim-two-views.md). Pure, host-tested (tests/anim_choice_test.cpp).
//
// The judge (EndSkelMeshComp_AnimOptimizationEval) turns a mesh's distance and angle to ONE camera snapshot into its
// update mode, its bone refresh and its cloth and hair (Bonamik) mode, with thresholds written into the code:
//   full update every frame  unless (angle >= 60 deg and distance >= 5 m) or distance >= 100 m
//   bones refreshed          unless distance >= 20 m and angle >= 60 deg (and the mode is not full)
//   cloth and hair reduced   when angle >= 60 deg and distance >= 5 m
//   significance             max(100 m - distance, 0), a tenth of it outside the cone
// Tier() names the best of these a camera gives; the mesh is judged from a further player's camera when its tier is
// higher, or the same with a higher significance. A tie keeps player 1's (the game's own choice), then the lower seat's.
//
// The snapshot is the allocator's block at +0xDC..+0xFB, in its order: the location predicted 0.1 s ahead, the forward
// predicted 1.3 turns ahead, the speed, the turn rate. Predict() is the game's formula (AnimBudgetAllocator_Update_SE),
// fed with a further player's camera manager instead of player 1's: the one recreated piece, the input only.

#include <algorithm>
#include <cmath>

namespace lss::world::anim_choice {

struct Vec
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec   operator-(const Vec& a, const Vec& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec   operator+(const Vec& a, const Vec& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec   operator*(const Vec& a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(const Vec& a, const Vec& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(const Vec& a) noexcept { return std::sqrt(Dot(a, a)); }
inline Vec   Cross(const Vec& a, const Vec& b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline constexpr float kHalfCone = 1.0471976f;  // 60 degrees, in radians
inline constexpr float kNear     = 500.0f;      // cm
inline constexpr float kBones    = 2000.0f;
inline constexpr float kFar      = 10000.0f;

/// A mesh's distance (cm) and angle (radians) to one camera.
struct View
{
    float distance = 0.0f;
    float angle    = 0.0f;
};

inline View Measure(const Vec& point, const Vec& camera, const Vec& forward) noexcept
{
    const Vec   to = point - camera;
    const float d  = Length(to);
    if (d <= 0.0f)
    {
        return {0.0f, 0.0f};
    }
    return {d, std::acos(std::clamp(Dot(to * (1.0f / d), forward), -1.0f, 1.0f))};
}

/// 2: full update every frame; 1: bones refreshed; 0: neither.
inline int Tier(const View& v) noexcept
{
    if ((v.angle < kHalfCone || v.distance < kNear) && v.distance < kFar)
    {
        return 2;
    }
    return v.distance < kBones || v.angle < kHalfCone ? 1 : 0;
}

inline float Significance(const View& v) noexcept
{
    return std::max(kFar - v.distance, 0.0f) * (v.angle >= kHalfCone ? 0.1f : 1.0f);
}

/// The mesh is judged from the second camera (player 2's, or any later one) rather than the first.
inline bool PreferTwo(const View& one, const View& two) noexcept
{
    const int a = Tier(one), b = Tier(two);
    return b > a || (b == a && Significance(two) > Significance(one));
}

/// The camera that sees the mesh best among `count` views, as an index: 0 is player 1's (the game's own), the later
/// ones the further players' in seat order. A later camera is taken only when it sees the mesh better (PreferTwo),
/// so a tie keeps the earlier one: player 1's, then the lower seat's.
inline int Best(const View* views, int count) noexcept
{
    int best = 0;
    for (int i = 1; i < count; ++i)
    {
        if (PreferTwo(views[best], views[i]))
        {
            best = i;
        }
    }
    return best;
}

/// The allocator's snapshot block (+0xDC..+0xFB), field for field.
struct Snapshot
{
    Vec   location;  // predicted
    Vec   forward;   // predicted
    float speed = 0.0f;
    float turn  = 0.0f;  // rad/s
};
static_assert(sizeof(Snapshot) == 32, "the allocator's block is 32 bytes");

/// What the prediction keeps between frames (the allocator's +0xC4, +0xD0 and +0xFC, kept per further player).
struct History
{
    Vec  location;
    Vec  forward;
    bool cut   = false;
    bool valid = false;
};

/// FRotator(pitch, yaw, roll).Vector(), degrees.
inline Vec ForwardOf(float pitchDegrees, float yawDegrees) noexcept
{
    constexpr float kRad = 3.14159265f / 180.0f;
    const float     p = pitchDegrees * kRad, y = yawDegrees * kRad;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}

/// This frame's snapshot from the camera's location, forward and cut flag, and `history` moved on: the game's formula.
inline Snapshot Predict(History& history, const Vec& location, const Vec& forward, bool cut, float dt) noexcept
{
    Snapshot   s;
    const bool reset = cut || history.cut || !history.valid || dt < 1e-8f || Length(history.forward) == 0.0f;
    if (reset)
    {
        s.location = location;
        s.forward  = forward;
    }
    else
    {
        const Vec velocity = (location - history.location) * (1.0f / dt);
        s.speed            = Length(velocity);
        s.location         = location + velocity * 0.1f;
        const float a = Length(history.forward), b = Length(forward);
        const float theta = std::acos(std::clamp(Dot(history.forward, forward) / (a * b), -1.0f, 1.0f));
        s.turn            = theta / dt;
        const Vec   axis  = Cross(history.forward, forward);
        const float n     = Length(axis);
        if (n < 1e-4f)
        {
            s.forward = forward;
        }
        else
        {
            // The previous forward turned 1.3 times this frame's turn about the same axis (perpendicular to it).
            const Vec   k   = axis * (1.0f / n);
            const float phi = 1.3f * theta;
            s.forward       = history.forward * std::cos(phi) + Cross(k, history.forward) * std::sin(phi);
        }
    }
    history = {location, forward, cut, true};
    return s;
}

}  // namespace lss::world::anim_choice
