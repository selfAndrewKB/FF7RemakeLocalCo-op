#pragma once

// Which XInput users have a pad, asked of the game's own xinput1_3 once a second on the game thread (the game's input
// system polls the same API every frame; a disconnected slot's query is the slow one, so never per frame). The
// game's pad records (engine_facts.h kStickBanks) carry no connected flag, so without this a dead pad beside a working
// one cannot be told from a scene or a lock in the log.
//   pads      the connected set, and per pad the packet number with the time since it last changed (a connected
//             pad that reports nothing is still "connected"; its packet number stands still)
// A change of the connected set logs at Debug. Each poll's set goes to players/pad_table's RecheckAnswering, which
// has the engine read again a slot whose pad answers while the engine counts it empty. Game thread only; nothing here
// reads or writes game memory.

namespace lss::players::pad_presence {

/// The `pads` reading.
void Register();

/// Once per frame on the game thread; asks XInput once a second.
void Tick() noexcept;

}  // namespace lss::players::pad_presence
