#pragma once

// Every player's area stays loaded and solid, not only player 1's (the user, 2026-10-29: "The entire world and much of
// its lighting unloads around P2 and they can no longer move until P1 returns"; research/RE-world-streaming-levers.md
// §1, §2, §4; S-RMK-STREAM's lever A, the planner set by id, loads nothing of the ground and is not used).
//
// The game streams the world by player 1's area alone: Square Enix's EnvironmentSet manager traces down from the party
// leader, and on a change of the area's set it hands that set's level list, one byte per level (0 shown, 1 hidden,
// 2 hidden with its collision kept), to its slot-4 level request. A Blueprint drains the request: what the new list
// adds is loaded, what it drops is unloaded (and hidden at once), so the list is residency and visibility at once.
// The mod makes that one request every player's: the game's call is redirected and handed the union of player 1's list
// and each further player's area's list (world/env_sets reads those with the game's own trace and read), merged by
// world/area_levels_rules.h (each level once, at most 256, shown wins over hidden). When a further player's area
// changes and player 1's does not, the mod asks the game's same function once more, with the same slot, priority and
// rule, from inside the manager's tick (env_sets tells it after each read); a list equal to the last one is not asked
// for again. The game's diff, refcounts, load order and hides do the rest.
// Player 1's request is passed through untouched (single player costs nothing and changes nothing) when no further
// player's area adds a level or a byte, while the game's hold mode stands (one scripted set for everyone, and the field
// exit's empty list), and when player 1's own list is past the request's room. A further player's area is dropped from
// the next request at once when that player leaves or the game holds (world/env_set_view.h). The mod's own requests
// build only on a list the game made outside a hold, in this world and this field: after a hold, a field's exit (the
// step machine leaving its steady step; fields are sub-levels of one world) or a new world, the game's own next request
// comes first. `env-sets off` stops the further players' view hides only; their areas still load.
// Not built yet (logged): the per-view hides of a level that one player's area hides and another's shows (it shows in
// both views; counted per player in the request line), and the further players' planner levels (triggers, nav walls,
// minimap volumes: lever A' of the research).
// Logged: player 1's list as the game asks for it (Info, from level_watch), each merged or own request with what it
// added and dropped (Info), a cut (Info). `area-levels on|off` (on); reading `world.area-levels`.
// Game thread: the game calls the slot-4 request inside the manager's tick, where env_sets' read runs too. The call's
// five bytes cross a 16-byte block, so Tick places the redirect from the game thread outside the world tick.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::area_levels {

/// Checks the request function and its call against the research and registers the switch and the reading; the
/// redirect is placed by Tick. False (logged) if anything differs: the world then follows player 1's area alone.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick outside the world tick: places the redirect on the first call.
void Tick() noexcept;

/// Game thread, inside the manager's tick right after env_sets has read every further player's area: asks the game
/// for the merged list when it differs from the last request (`changed`: a further player's area changed this read).
void SeatsRead(bool changed) noexcept;

/// The redirect stays in place and passes the game's lists through from here (any thread).
void Remove() noexcept;

/// The `world.area-levels` reading: the switch, the last request, its parts, the requests made.
std::wstring Reading() noexcept;

}  // namespace lss::world::area_levels
