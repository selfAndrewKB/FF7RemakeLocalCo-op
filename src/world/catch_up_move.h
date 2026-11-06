#pragma once

// The game's own way to put a character beside another on the field, as the modules that bring a character over call
// it (world/talk_gather, world/run_catch_up, world/party_warp; research/RE-field-catchup-warp.md):
//   - a navmesh point a distance from an origin along a flat direction: the game's query, with the shape its field
//     catch-up warps use (the plain field filter, no area ids, the default agent). It tests a path from the origin
//     and never answers a point on another character;
//   - the game's battle-warp move on a character there: its party teleport (the capsule placed standing on the
//     point, the character's own trigger sweep re-based at the landing, its nav area refreshed) and a short fade-in.
//     The landing and the walk target are the same point, so the character stands there at once.
// Nothing else of the battle warp is made: its control-layer bits can leave a field character unable to move.
// The move reads "the party leader" (a leader gets the leader-only steps): call it outside every player's scope.
// Found and verified once at install; the callers check Installed() at theirs. Game thread.

#include "world/actor_place.h"

namespace lss::platform {
class GameImage;
}

namespace lss::world::catch_up_move {

/// Finds the query, the move and the default agent. False (logged) when one is not as the research read it; the
/// callers then stand aside.
bool Install(const platform::GameImage& image) noexcept;
bool Installed() noexcept;

/// A navmesh point `distanceCm` from `origin` along the flat `dir`; false when the game finds none or the call raises.
bool NavPoint(const actor_place::Vec3& origin, const actor_place::Vec3& dir, float distanceCm, actor_place::Vec3& out) noexcept;

/// Moves `character` to stand on `point`, facing `yawDegrees`, with the game's fade-in. False when the call raised
/// (the character may stand where it was).
bool Move(void* character, const actor_place::Vec3& point, float yawDegrees) noexcept;

/// The character runs a field action (a ladder, a crawl, the grappling hook): not to be moved out of it.
bool InFieldAction(const void* character) noexcept;

/// A following companion's distance from the character it is brought to.
inline constexpr float kFollowerCm = 150.0f;

/// A further player's character other than `except` stands within 1 m of `at`: not a spot to bring `except` to. The
/// query keeps its point off every character it finds; this second test covers a character moved earlier in the same
/// tick, which the query may not count yet [ASSUMPTION].
bool SpotTaken(const actor_place::Vec3& at, const void* except) noexcept;

}  // namespace lss::world::catch_up_move
