#pragma once

// NPCs notice player 2 as they notice player 1, player 1 first (research/RE-npc-reactions.md; the user, 2026-10-21:
// "some NPCs that programmed to respond to P1's presence as they walk by ... expand to P2, while still giving P1
// priority").
//
// The turn an NPC makes toward a passer-by is Square Enix's own NPC model: its Curiosity look takes the most
// interesting character it sees within 2 m and keeps it to the end. It picks any character, player 2's included
// (read live: NPCs near player 2 know her and have already looked at her). Two things still favoured player 1 alone
// or not at all:
//   1. Sight works through one grid of 4 m cells (15 x 15 x 3) that the game centres on player 1's character every
//      field tick; a character outside it is seen by no NPC. World/npc_senses_code puts the grid's centre, for that
//      tick only, part of the way toward player 2 when player 2 would fall outside, never so far that player 1 comes
//      within a cell of the edge, never in height and never in battle (the grid also holds the battle heat map). The
//      choice of centre is the one recreated piece (no game path exists; documented in the plan's world topic).
//   2. When both players qualify for the same NPC's look in one update, the game takes the more interesting (the
//      nearer). Player 1 first: the mod runs the game's own pick once with only player 1 eligible (every other
//      candidate's "seen this update" timer set aside for that one call and put back exactly); if that pick finds
//      nobody (player 1 out of the look's reach), the game's own pick runs as it would have. A look already running
//      is never taken over. That pick has no effect when it finds nobody (read in IDA), so the second run is the
//      game's single pick. Game thread only; any other thread gets the game's pick unchanged (counted).
// Everything else (the senses, the interest, the look, the animation) is the game's.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::npc_senses {

/// Installs the grid code and the look's slot (each checked against the research; logged). False if neither could.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick: player 1's character (player controller 0's pawn) and player 2's character while
/// player 2 is in (null otherwise).
void Tick(void* memberOne, void* memberTwo) noexcept;

/// The switch (`npc-senses on|off`, on at start): off leaves the grid on player 1 and the look to the game alone.
void SetEnabled(bool on) noexcept;

/// The `world.npc-senses` reading: where the grid is, the looks given to player 1 first.
std::wstring Reading() noexcept;

}  // namespace lss::world::npc_senses
