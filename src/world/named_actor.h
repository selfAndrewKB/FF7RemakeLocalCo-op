#pragma once

#include <cstdint>

// A character or object found by its name as the game finds it: the character registry's per-level name map
// (CharaRegistry_FindActorByName). The game's own field-target dispatch, named-point resolver and scripted-move writer
// pass the level FName kept at kNamedActorLevel, read at each call; the environment sets pass the levels they apply to.
// Set at install, then only read; the lookups are guarded calls, safe on any thread the game's own callers use.

namespace lss::platform {
class GameImage;
}

namespace lss::world::named_actor {

/// Checks the registry lookup and the level name against the research. False (logged) when a check fails.
bool Install(const platform::GameImage& image) noexcept;
bool Installed() noexcept;

/// The actor `name` in `level` (FName bits), or null when not found, the lookup is not installed or the call faulted.
/// Liveness is the caller's: the registry can still list an actor being destroyed.
void* InLevel(std::uint64_t level, std::uint64_t name) noexcept;

/// The actor `name` in the level the game's own name lookups use, or null when not found or dying.
void* Find(std::uint64_t name) noexcept;

}  // namespace lss::world::named_actor
