#pragma once

// Logging only: what loads, shows, hides and unloads the world's levels, so the further players' areas can be held by
// the game's own loader (the user, 2026-10-27: "we'll take the native path for world loading. prepare the logging";
// research/RE-world-streaming-players.md, lever A and the live checks of §8).
//
// The game decides residency and visibility from player 1's area alone: Square Enix's EnvironmentSet manager traces
// down from the party leader, and on a change of the area set it requests that set's level list on visibility slot 4
// (levels marked in the parallel bytes are hidden: reason 5, actors only, the level stays loaded) and puts the area's
// planner level set into residency slot 32 (the reference-counted loader adds the new set, then releases the old; a
// level's last release enqueues its unload). What the user sees behind player 1 is one or the other; these lines tell
// which:
//   - player 1's area change: the slot-32 set, old and new, with each further player's own ground set beside it (Info:
//     a state change); the slot-4 list with each level's byte is logged by world/area_levels, which owns that call
//     (one redirect per call site) and adds the further players' areas to it;
//   - every request into the game's level queue: load or unload, the level, the priority (Debug per event);
//   - the world's streaming levels, compared each engine tick: a level that starts or stops being wanted loaded or
//     shown, or whose level object comes or goes (Debug per change; the first look at a world is one Info line with its
//     counts);
//   - every 10 s: the counts, the process's committed memory and the census's own time per tick (Debug), for the memory
//     check before the further players' areas are held.
// The census runs only while a further player is in (single player costs nothing).
// Game thread for the census; the two redirects run where the game calls them (the area change in the world tick, the
// queue wherever a load is asked for; logging only, safe on any thread).
// `level-watch on|off` (on), reading `world.levels`.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::level_watch {

/// Checks the calls and functions against the research, places the slot-32 call redirect and the queue's entry redirect,
/// registers the switch and the reading. False (logged) if anything differs: nothing is watched then.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick outside the world tick: compares the world's streaming levels with the last tick's.
/// `anyIn`: a further player is in (the 10 s memory line).
void Tick(bool anyIn) noexcept;

/// Puts the slot-32 call and the queue's entry back (any thread: one block each).
void Remove() noexcept;

/// The `world.levels` reading: the counts, the last area sets, the requests seen, the committed memory.
std::wstring Reading() noexcept;

}  // namespace lss::world::level_watch
