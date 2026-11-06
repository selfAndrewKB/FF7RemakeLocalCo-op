#pragma once

// A further player's character brought back beside player 1's in the field (world/party_warp.h applies it), as pure
// rules: when the ask is refused, and where the character is put. The refusals are the game's own gates for its
// follower's catch-up warp (no scene or event holding the field, the leader in no field action) and the mod's own for a
// character a player holds (it belongs to its player, in the field mode, in no field action of its own, not already
// beside player 1's character). The spots: behind
// player 1's character first, as the game's own catch-up warps place a follower, then either side, then in front.
// Host-tested (tests/party_warp_rules_test.cpp).

#include "world/actor_place.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace lss::world::party_warp_rules {

using actor_place::Vec3;

/// What the ask is judged by, as read at the engine tick. The defaults are free play's: a caller fills every field it
/// reads, and judges the fields it has not read yet as free (world/party_warp.cpp asks twice: the pause and the scene
/// before either character is read, the rest after).
struct Facts
{
    bool          paused           = false;  // the game is paused or a menu owns the input
    std::uint32_t fieldLock        = 0;      // the field control lock's mask (a scene, a talk, an event)
    bool          leaderWithPlayer = true;   // player 1's character is with its player (not handed to its AI)
    bool          memberWithPlayer = true;   // the asking player's character is with its player (no scripted hold)
    bool          memberInField    = true;   // its actor mode is the field's (not a cutscene's or a battle's)
    bool          memberBusy       = false;  // it runs a field action (a ladder, a crawl, the grappling hook)
    bool          leaderBusy       = false;  // player 1's character runs one
    bool          beside           = false;  // it already stands beside player 1's character (Beside)
};

inline constexpr float kNearCm  = 300.0f;  // beside, in the ground plane
inline constexpr float kLevelCm = 150.0f;  // and in height: a floor above or below is not beside

/// The character already stands beside player 1's. A double click there moves nothing.
constexpr bool Beside(float flatCm, float heightCm) noexcept
{
    return flatCm < kNearCm && heightCm < kLevelCm && heightCm > -kLevelCm;
}

/// Why the character is not brought back; null when it may be.
constexpr const wchar_t* Refusal(const Facts& facts) noexcept
{
    if (facts.paused)
    {
        return L"the game is paused or a menu is up";
    }
    if (facts.fieldLock != 0 || !facts.leaderWithPlayer)
    {
        return L"a scene, a talk or an event holds the party";
    }
    if (!facts.memberWithPlayer || !facts.memberInField)
    {
        return L"the story holds that character";
    }
    if (facts.memberBusy)
    {
        return L"it is in a field action (a ladder, a crawl)";
    }
    if (facts.leaderBusy)
    {
        return L"player 1's character is in a field action (a ladder, a crawl)";
    }
    if (facts.beside)
    {
        return L"it already stands beside player 1's character";
    }
    return nullptr;
}

constexpr std::size_t kTries = 4;

/// The flat unit directions from player 1's character to try for a navmesh point, in order: behind it, to its left, to
/// its right, in front. `yawDegrees` is its facing (an FRotator's yaw: 0 along +X, 90 along +Y).
inline std::array<Vec3, kTries> Spots(float yawDegrees) noexcept
{
    const float yaw = yawDegrees * std::numbers::pi_v<float> / 180.0f;
    const Vec3  forward{std::cos(yaw), std::sin(yaw), 0.0f};
    const Vec3  right{-forward.y, forward.x, 0.0f};
    return {Vec3{-forward.x, -forward.y, 0.0f}, Vec3{-right.x, -right.y, 0.0f}, right, forward};
}

/// The try's name for the log line.
constexpr const wchar_t* SpotName(std::size_t used) noexcept
{
    constexpr const wchar_t* kNames[kTries] = {L"behind it", L"to its left", L"to its right", L"in front of it"};
    return used < kTries ? kNames[used] : L"nowhere";
}

}  // namespace lss::world::party_warp_rules
