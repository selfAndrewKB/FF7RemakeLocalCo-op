#pragma once

// Where an actor stands, read the one way: its root component's world location, guarded (null actor, null root or
// a faulting read answer false, `out` untouched). The distance the modules measure with comes with it: flat, in the
// ground plane (the talk gather's threshold, the battle warps' log). Before this header nine modules each read the two fields
// themselves (talk_gather's Vec3/Location/Flat, battle_warps' FlatMetres, npc_senses' and npc_crowd's Location,
// traversal_walkup's LocationOf, field_icons' and anim_views' owner location, env_sets's trace origin,
// npc_detail's record walk). The modules whose arithmetic takes arrays or
// their own three-float struct keep that type and take the array form; audio/listener_rules keeps its std::array
// form, since its host-tested math takes arrays. Any thread: a guarded read of the actor's own fields, nothing kept.

#include <cmath>

namespace lss::world::actor_place {

struct Vec3
{
    float x = 0, y = 0, z = 0;
};

/// The actor's root component's world location (UE centimetres); false, `out` untouched, when it cannot be read.
bool ActorLocation(const void* actor, Vec3& out) noexcept;
bool ActorLocation(const void* actor, float (&out)[3]) noexcept;

/// The actor's facing: its root component's yaw in degrees (an FRotator's; a character's root has no parent, so its
/// relative rotation is the world's); false, `out` untouched, when it cannot be read.
bool ActorYaw(const void* actor, float& out) noexcept;

/// The distance in the ground plane, in the actors' own units (centimetres).
inline float FlatDistance(const Vec3& a, const Vec3& b) noexcept
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

}  // namespace lss::world::actor_place
