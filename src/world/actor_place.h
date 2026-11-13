#pragma once

// Where an actor stands, read the one way: its root component's world location, guarded (null actor, null root or
// a faulting read answer false, `out` untouched). The distance the modules measure with comes with it: flat, in the
// ground plane (the talk gather's threshold, the battle warps' log). Every module that needs an actor's place reads it
// here. The location is the engine's FVector (platform/game_vectors.h); the array form serves the modules whose
// arithmetic takes arrays (audio/listener_rules' host-tested math among them). Any thread: a guarded read of the
// actor's own fields, nothing kept.

#include "platform/game_vectors.h"

#include <cmath>

namespace lss::world::actor_place {

using platform::GameVector;

/// The actor's root component's world location (UE centimetres); false, `out` untouched, when it cannot be read.
bool ActorLocation(const void* actor, GameVector& out) noexcept;
bool ActorLocation(const void* actor, float (&out)[3]) noexcept;

/// The actor's facing: its root component's yaw in degrees (an FRotator's; a character's root has no parent, so its
/// relative rotation is the world's); false, `out` untouched, when it cannot be read.
bool ActorYaw(const void* actor, float& out) noexcept;

/// The distance in the ground plane, in the actors' own units (centimetres).
inline float FlatDistance(const GameVector& a, const GameVector& b) noexcept
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

}  // namespace lss::world::actor_place
