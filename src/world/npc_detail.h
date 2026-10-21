#pragma once

// The NPCs player 2 is near keep the detail the game would give them from player 2's own camera (research/
// RE-npc-near-player-two.md §1; Rebirth's player_two/charapop_near, cleanup-2026-10).
//
// The game's NPC detail (CharaPop) judges every entry from player 1's camera only: an NPC beside player 2 pauses as
// soon as it is past a few metres outside player 1's view cone, and the ones that hide by distance fade out. The
// game has no path for a second view; its own per-entry lever is the minimum level, which event scripts and field-AI
// animations set with CharaPop_SetEntryMinLODLevel_ByName (3 keeps an entry awake, shown and fully animated; 0 clears
// it). While player 2 is in, the mod calls it the same way for the entries player 2's view needs.
//
// The one recreated piece, documented in the plan's §2: which entries player 2's view needs. It is the evaluator's own
// rule (world/charapop_rule.h) with the game's own distances and overrides read from the manager, applied to player
// 2's camera instead of player 1's. Only entries player 1's camera does not already keep are raised, so with the
// players together almost nothing is. A raised entry is released after about a second outside player 2's reach, or
// well inside player 1's, so an entry on the edge does not toggle.
//
// Never touches an entry whose minimum the game set (only entries reading 0 are raised), and clears only entries the
// mod raised that still read 3. Everything raised is cleared when player 2 leaves; the records are dropped without
// calls when the map's CharaPop changes. Game thread only.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::npc_detail {

/// Finds the game's setter; logged either way. False if a check failed (nothing is raised then).
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick: both players' camera managers while player 2 is in (null otherwise).
void Tick(void* cameraManagerOne, void* cameraManagerTwo) noexcept;

/// The comparison switch (`npc-detail on|off`, on at start). Off clears what was raised.
void SetEnabled(bool on) noexcept;

/// The `world.npc-detail` reading: what is raised now and the counts so far.
std::wstring Reading() noexcept;

}  // namespace lss::world::npc_detail
