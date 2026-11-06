#pragma once

// The NPCs a further player is near keep the detail the game would give them from that player's own camera
// (research/RE-npc-near-player-two.md): the reach is the union of every further player's view.
//
// The game's NPC detail (CharaPop) judges every entry from player 1's camera only: an NPC beside player 2 pauses as
// soon as it is past a few metres outside player 1's view cone, and the ones that hide by distance fade out. The
// game has no path for a second view; its own per-entry lever is the minimum level, which event scripts and field-AI
// animations set with CharaPop_SetEntryMinLODLevel_ByName (3 keeps an entry awake, shown and fully animated; 0 clears
// it). While a further player is in, the mod calls it the same way for the entries their views need.
//
// The one recreated piece, documented in the plan: which entries the further players' views need. It is the
// evaluator's own rule (world/charapop_rule.h) with the game's own distances and overrides read from the manager,
// applied to each further player's camera instead of player 1's. Only entries player 1's camera does not already
// keep are raised, so with the players together almost nothing is. A raised entry is released after about a second
// outside every further player's reach, or well inside player 1's, so an entry on the edge does not toggle.
//
// Never touches an entry whose minimum the game set (only entries reading 0 are raised), and clears only entries the
// mod raised that still read 3. Everything raised is cleared when the last further player leaves; the records are
// dropped without calls when the map's CharaPop changes. Game thread only.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::npc_detail {

/// Finds the game's setter; logged either way. False if a check failed (nothing is raised then).
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick, per built seat: the seat's camera manager while its player is in (null otherwise);
/// the pointer is kept until the seat's next call and read only through guarded reads.
void TickSeat(int seat, void* seatCameraManager) noexcept;

/// Game thread, each engine tick after the seats': player 1's camera manager while a further player is in (null
/// otherwise); runs the pass every few ticks.
void Tick(void* cameraManagerOne) noexcept;

/// The comparison switch (`npc-detail on|off`, on at start). Off clears what was raised.
void SetEnabled(bool on) noexcept;

/// The `world.npc-detail` reading: what is raised now and the counts so far.
std::wstring Reading() noexcept;

}  // namespace lss::world::npc_detail
