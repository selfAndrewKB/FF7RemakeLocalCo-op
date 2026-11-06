#pragma once

// The game's NPC crowd follows every player: each NPC is scored from the nearest player (research/
// RE-npc-near-player-two.md §2; Rebirth's player_two/charapop_centre, its "approach B"; the seats
// follow plan/seats.md step 3).
//
// CharaPop keeps a budget of NPCs spawned, ranked by a score whose distance part is measured from one centre: the
// party leader, or a point an event script sets (event command 0x7E). Far from that centre an NPC is despawned or
// never spawns, so the townspeople near player 2 were missing while player 1 was far. The game has one centre and no
// path for a second.
//
// While a further player is in, the mod replaces the distance score's vtable slot: the game's own score runs as is,
// then once more for each seat whose player holds a member, with that member as the centre, and the highest is
// returned (the nearest player's). Remake has no setter for the centre, so for each further call the mod writes the
// centre fields the event task itself writes (the byte and the point, 16 bytes) and puts them back at once; only the
// centre reader reads them. The evaluator's one side effect (its last non-zero score) is set as the game would have
// left it. Everything after the score (the sort, the budget, the spawn, the despawn) is the game's, so each player
// gets the single-player crowd around them and nothing is spawned for the empty space between them. An event's centre
// keeps the game's single score. Game thread only (the score runs only in the field tick).

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::npc_crowd {

/// Replaces the score's slot (checked; logged). False if it differs.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick, per built seat: the seat's character while its player is in (null otherwise).
void TickSeat(int seat, void* seatMember) noexcept;

/// Game thread, each engine tick after the seats': the minute's summary.
void Tick() noexcept;

/// The comparison switch (`npc-crowd on|off`, on at start).
void SetEnabled(bool on) noexcept;

/// The `world.npc-crowd` reading: scores taken from each further player over the last minute.
std::wstring Reading() noexcept;

}  // namespace lss::world::npc_crowd
