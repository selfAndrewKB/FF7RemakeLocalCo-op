#include "world/actor_place.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "players/actor_fields.h"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace lss::world::actor_place {

bool ActorLocation(const void* actor, float (&out)[3]) noexcept
{
    const void* root = players::actor_fields::RootComponent(actor);
    float       at[3]{};  // read apart from `out`, so a fault partway leaves `out` untouched
    if (root == nullptr || !platform::Read(static_cast<const std::uint8_t*>(root) + facts::kComponentWorldLocation, at))
    {
        return false;
    }
    std::copy(std::begin(at), std::end(at), out);
    return true;
}

bool ActorLocation(const void* actor, GameVector& out) noexcept
{
    float at[3]{};
    if (!ActorLocation(actor, at))
    {
        return false;
    }
    out = {at[0], at[1], at[2]};
    return true;
}

bool ActorYaw(const void* actor, float& out) noexcept
{
    const void* root = players::actor_fields::RootComponent(actor);
    float       rotation[3]{};  // pitch, yaw, roll
    if (root == nullptr || !platform::Read(static_cast<const std::uint8_t*>(root) + facts::kSceneRelativeRotation, rotation))
    {
        return false;
    }
    out = rotation[1];
    return true;
}

}  // namespace lss::world::actor_place
