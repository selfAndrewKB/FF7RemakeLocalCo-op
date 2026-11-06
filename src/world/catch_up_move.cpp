#include "world/catch_up_move.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <array>
#include <cstdint>

namespace lss::world::catch_up_move {

namespace {

using actor_place::Vec3;

using NavPointFn = bool (*)(float* out, const float* origin, const float* dir, float distance, const void* agent,
                            std::uint64_t, std::uint8_t, std::uint32_t, std::uint64_t);
using WarpFn     = void (*)(void* character, const float* location, const float* rotation, const float* direct, bool fade);

// Set at install, then only read.
std::uintptr_t g_agent    = 0;
NavPointFn     g_navPoint = nullptr;
WarpFn         g_warp     = nullptr;

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto navPoint = platform::FindFunction(image, facts::kNavPointAlongDir);
    const auto warp     = platform::FindFunction(image, facts::kBattleTeleportFadeIn);
    const auto agent    = image.Rebase(facts::kNavAgentDefault, facts::kNavAgentSize);
    if (!navPoint || !warp || !agent)
    {
        platform::Log(L"catch-up move: an engine check failed; no character is brought beside another (a talk another "
                      L"player starts, a companion's story run, a player's own ask)");
        return false;
    }
    g_agent    = *agent;
    g_navPoint = reinterpret_cast<NavPointFn>(*navPoint);
    g_warp     = reinterpret_cast<WarpFn>(*warp);  // last: Installed tests it
    return true;
}

bool Installed() noexcept
{
    return g_warp != nullptr;
}

bool NavPoint(const Vec3& origin, const Vec3& dir, const float distanceCm, Vec3& out) noexcept
{
    if (!Installed())
    {
        return false;
    }
    alignas(16) std::array<std::uint8_t, facts::kNavAgentSize> agent{};
    alignas(16) float point[8]  = {};  // room for an FNavLocation (location, padding, node), not only an FVector
    const float       from[3]   = {origin.x, origin.y, origin.z};
    const float       toward[3] = {dir.x, dir.y, 0.0f};
    bool              found     = false;
    if (!platform::CopyGuarded(agent.data(), reinterpret_cast<const void*>(g_agent), agent.size()) ||
        !platform::CallGuarded(g_navPoint, found, point, from, toward, distanceCm, static_cast<const void*>(agent.data()),
                               std::uint64_t{0}, std::uint8_t{0}, std::uint32_t{0}, std::uint64_t{0}) ||
        !found || (point[0] == 0.0f && point[1] == 0.0f && point[2] == 0.0f))
    {
        return false;
    }
    out = {point[0], point[1], point[2]};
    return true;
}

bool Move(void* character, const Vec3& point, const float yawDegrees) noexcept
{
    if (!Installed() || character == nullptr)
    {
        return false;
    }
    const float location[3] = {point.x, point.y, point.z};
    const float rotation[3] = {0.0f, yawDegrees, 0.0f};  // an FRotator: pitch, yaw, roll in degrees
    return platform::CallGuarded(g_warp, character, static_cast<const float*>(location), static_cast<const float*>(rotation),
                                 static_cast<const float*>(location), true);
}

bool InFieldAction(const void* character) noexcept
{
    const void*  component = platform::ReadPointer(character, facts::kCharacterFieldAction);
    std::uint8_t running   = 0;
    return component != nullptr &&
           platform::Read(static_cast<const std::uint8_t*>(component) + facts::kFieldActionRunning, running) && running != 0;
}

bool SpotTaken(const Vec3& at, const void* except) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const void* other = players::scopes::Member(seat);
        Vec3        there{};
        if (other != nullptr && other != except && actor_place::ActorLocation(other, there) &&
            actor_place::FlatDistance(at, there) < 100.0f)
        {
            return true;
        }
    }
    return false;
}

}  // namespace lss::world::catch_up_move
