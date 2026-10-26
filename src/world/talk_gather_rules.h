#pragma once

// Where player 1's character is brought when another player starts a talk (world/talk_gather.h applies it), as a pure
// rule: the flat directions to try for a navmesh point beside the presser, in order, and whom the moved character
// faces. With the NPC's place known and within the game's own "far" of the presser: the side of the presser's line
// to the NPC that faces where player 1's character stands, else the other side, else behind the presser (the user,
// 2026-10-26: "just beside", facing the NPC). Without it: toward player 1's character, else either side of that line,
// facing the presser (research/RE-talk-far.md; the 14:28 live miss of v0.102.0 was this rule's input, not the rule).
// Host-tested (tests/talk_gather_rules_test.cpp). Cleanup step 4, plan/code-alignment.md C3.

#include "world/actor_place.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace lss::world::talk_gather_rules {

using actor_place::Vec3;

constexpr float kFarCm    = 800.0f;  // the game's own "far from the leader" (the battle end's warp threshold)
constexpr float kBesideCm = 120.0f;  // shoulder to shoulder with the presser (the user, 2026-10-26: "Just beside.")
constexpr float kPi       = 3.14159265358979f;

struct Beside
{
    std::array<Vec3, 3> tries{};         // flat unit directions from the presser, in the order to try
    bool                withNpc = false;  // the NPC's place counted (known, more than 1 cm and at most kFarCm away)
    Vec3                face{};           // whom the moved character faces: the NPC, else the presser
};

/// The flat unit vector from `from` toward `to`; (1, 0, 0) when they coincide.
inline Vec3 Toward(const Vec3& from, const Vec3& to) noexcept
{
    const float span = actor_place::FlatDistance(from, to);
    return span > 0.0f ? Vec3{(to.x - from.x) / span, (to.y - from.y) / span, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
}

/// The rule. `npc` null when the NPC's place is unknown.
inline Beside BesideSpot(const Vec3& presser, const Vec3& cloud, const Vec3* npc) noexcept
{
    Beside     beside;
    const Vec3 toCloud = Toward(presser, cloud);
    beside.tries       = {toCloud, Vec3{-toCloud.y, toCloud.x, 0.0f}, Vec3{toCloud.y, -toCloud.x, 0.0f}};
    beside.face        = presser;
    if (npc != nullptr)
    {
        const float gap = actor_place::FlatDistance(*npc, presser);
        beside.withNpc  = gap > 1.0f && gap <= kFarCm;
    }
    if (beside.withNpc)
    {
        const Vec3 toNpc     = Toward(presser, *npc);
        const Vec3 left{-toNpc.y, toNpc.x, 0.0f};
        const bool cloudLeft = left.x * toCloud.x + left.y * toCloud.y >= 0.0f;
        const Vec3 nearSide  = cloudLeft ? left : Vec3{-left.x, -left.y, 0.0f};
        beside.tries         = {nearSide, Vec3{-nearSide.x, -nearSide.y, 0.0f}, Vec3{-toNpc.x, -toNpc.y, 0.0f}};
        beside.face          = *npc;
    }
    return beside;
}

/// The yaw, in degrees (an FRotator's), that faces `toward` from `at`.
inline float FacingYaw(const Vec3& at, const Vec3& toward) noexcept
{
    return std::atan2(toward.y - at.y, toward.x - at.x) * 180.0f / kPi;
}

/// The try's name for the log line.
constexpr const wchar_t* SpotName(bool withNpc, std::size_t used) noexcept
{
    constexpr const wchar_t* kSpots[2][3] = {{L"toward where it stood", L"to one side", L"to the other side"},
                                             {L"on its side", L"on the far side", L"behind"}};
    return used < 3 ? kSpots[withNpc ? 1 : 0][used] : L"nowhere";
}

}  // namespace lss::world::talk_gather_rules
