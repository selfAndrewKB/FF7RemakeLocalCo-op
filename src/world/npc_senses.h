#pragma once

// NPCs notice every further player as they notice player 1, player 1 first (research/RE-npc-reactions.md): an NPC
// made to respond to player 1 walking by responds to any player, and to player 1 when several are near (the seats
// follow plan/seats.md step 3).
//
// The turn an NPC makes toward a passer-by is Square Enix's own NPC model: its Curiosity look takes the most
// interesting character it sees within 2 m and keeps it to the end. It picks any character, a further player's
// included (read live: NPCs near player 2 know her and have already looked at her). Two things still favoured player
// 1 alone or not at all:
//   1. Sight works through one grid of 4 m cells (15 x 15 x 3) that the game centres on player 1's character every
//      field tick; a character outside it is seen by no NPC. World/npc_senses_code puts the grid's centre, for that
//      tick only, part of the way toward the further players when one would fall outside, never so far that player 1
//      comes within a cell of the edge, never in height and never in battle (the grid also holds the battle heat
//      map); a further player who cannot sit in one grid with player 1 and the lower seats is left out. The choice of
//      centre is the one recreated piece (no game path exists; documented in the plan's world topic).
//   2. When player 1 and a further player qualify for the same NPC's look in one update, the game takes the more
//      interesting (the nearer). Player 1 first: the mod runs the game's own pick once with only player 1 eligible
//      (every other candidate's "seen this update" timer set aside for that one call and put back exactly); if that
//      pick finds nobody (player 1 out of the look's reach), the game's own pick runs as it would have. A look already
//      running is never taken over. That pick has no effect when it finds nobody (read in IDA), so the second run is
//      the game's single pick. The look runs in each NPC's own AI tick on worker threads (research/RE-npc-reactions.md
//      §10.5): the players are a snapshot the game thread publishes, and only that NPC's own entries are touched.
// Everything else (the senses, the interest, the look, the animation) is the game's.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::npc_senses {

/// Installs the grid code and the look's slot (each checked against the research; logged). False if neither could.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick, per built seat: the seat's character while its player is in (null otherwise).
void TickSeat(int seat, void* seatMember) noexcept;

/// Game thread, each engine tick after the seats': player 1's character (player controller 0's pawn); decides the
/// grid's centre for this tick and publishes the players to the look.
void Tick(void* memberOne) noexcept;

/// The switch (`npc-senses on|off`, on at start): off leaves the grid on player 1 and the look to the game alone.
void SetEnabled(bool on) noexcept;

/// The `world.npc-senses` reading: where the grid is, each further player's fit, the looks given to player 1 first.
std::wstring Reading() noexcept;

}  // namespace lss::world::npc_senses
