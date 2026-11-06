#pragma once

// Where player 1's character is brought when another player starts a talk (world/talk_gather.h applies it), as a pure
// rule: the flat directions to try for a navmesh point beside the presser, in order, and whom the moved character
// faces. With the NPC's place known and within the game's own "far" of the presser: the side of the presser's line
// to the NPC that faces where player 1's character stands, else the other side, else behind the presser (just beside,
// facing the NPC). Without it: toward player 1's character, else either side of that line,
// facing the presser (research/RE-talk-far.md).
// Host-tested (tests/talk_gather_rules_test.cpp).

#include "world/actor_place.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace lss::world::talk_gather_rules {

using actor_place::Vec3;

constexpr float kFarCm    = 800.0f;  // the game's own "far from the leader" (the battle end's warp threshold)
constexpr float kBesideCm = 120.0f;  // shoulder to shoulder with the presser: just beside
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

/// How long player 1's press of a target counts as the start of the talk that then takes the field lock: the game
/// queues the talk's act a tick or two after the press, and an act waits while another holds its lock bits.
constexpr std::uint64_t kPressFreshMs = 10000;

/// The action row types that are a talk's acts: the two planner events (a line exchange, a staged conversation) and
/// the show-menu act (the shop a shopkeeper's talk opens). Type 0 is a cutscene act; -1 an unread row.
inline constexpr std::int32_t kActPlanEvent = 1, kActPlanEventAlt = 16, kActShowMenu = 43;
constexpr bool TalkAct(std::int32_t type) noexcept
{
    return type == kActPlanEvent || type == kActPlanEventAlt || type == kActShowMenu;
}

/// Whether the act that took the field lock is player 1's own prompt talk: one of a talk's acts (`talkAct`: TalkAct of
/// its type) on the channel of the object player 1 pressed (`pressed` at `pressedAt`, 0: no press), within
/// kPressFreshMs. A talk whose data runs it on another object's channel is not recognised; it then plays as any scene
/// does.
constexpr bool PlayerOneTalk(std::uint64_t pressed, std::uint64_t pressedAt, std::uint64_t object, std::uint64_t now,
                             bool talkAct) noexcept
{
    return talkAct && object != 0 && pressed == object && now >= pressedAt && now - pressedAt <= kPressFreshMs;
}

/// The try's name for the log line.
constexpr const wchar_t* SpotName(bool withNpc, std::size_t used) noexcept
{
    constexpr const wchar_t* kSpots[2][3] = {{L"toward where it stood", L"to one side", L"to the other side"},
                                             {L"on its side", L"on the far side", L"behind"}};
    return used < 3 ? kSpots[withNpc ? 1 : 0][used] : L"nowhere";
}

}  // namespace lss::world::talk_gather_rules
