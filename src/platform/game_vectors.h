#pragma once

// The engine's FVector and FVector2D as this engine lays them out: 4-byte floats, packed, no padding. Pure.
//
// The one copy of the engine's layout in the mod. A world location read off a component, a point the game's projection
// writes, a size passed by value to a game function: each is one of these, so a build of an engine whose vectors differ
// (wider floats, other padding) is refused here instead of misread in one module. An FVector2D by value is 8 bytes, so
// the x64 convention passes it packed in one register, as the game's own callers do.
//
// The arithmetic is the plain component-wise kind the mod's rules need (a camera's distance and angle to a mesh, a turn
// between two forwards).

#include <bit>
#include <cmath>
#include <cstdint>

namespace lss::platform {

/// FVector: a world location or direction (UE centimetres for a location).
struct GameVector
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
static_assert(sizeof(GameVector) == 12 && alignof(GameVector) == 4, "the engine's FVector: three packed 4-byte floats");

/// FVector2D: a screen or layout point, or a size.
struct GameVector2D
{
    float x = 0.0f, y = 0.0f;
};
static_assert(sizeof(GameVector2D) == 8 && alignof(GameVector2D) == 4, "the engine's FVector2D: two packed 4-byte floats");

/// An FVector2D as a game function takes it by value: one register, x in its low half, y in its high.
inline std::uint64_t PassedByValue(const GameVector2D& v) noexcept
{
    return std::bit_cast<std::uint64_t>(v);
}

inline GameVector operator-(const GameVector& a, const GameVector& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline GameVector operator+(const GameVector& a, const GameVector& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline GameVector operator*(const GameVector& a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
inline float      Dot(const GameVector& a, const GameVector& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float      Length(const GameVector& a) noexcept { return std::sqrt(Dot(a, a)); }
inline GameVector Cross(const GameVector& a, const GameVector& b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

}  // namespace lss::platform
