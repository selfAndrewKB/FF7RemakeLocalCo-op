#pragma once

// Which XInput users have a pad, asked of XInput itself once a second on the game thread (the game's own input
// system polls the same API every frame; a disconnected slot's query is the slow one, so never per frame).
// Diagnostics only: the game's pad records (engine_facts.h kStickBanks) carry no connected flag, so a dead pad 2
// with a working pad 1 (the user's 2026-10-21 report) could not be told from a scene or a lock in the log.
//   pads      the connected set, and per pad the packet number with the time since it last changed (a connected
//             pad that reports nothing is still "connected"; its packet number stands still)
// A change of the connected set logs at Debug. Game thread only; no game memory is read or written.

namespace lss::players::pad_presence {

/// The `pads` reading.
void Register();

/// Once per frame on the game thread; asks XInput once a second.
void Tick() noexcept;

}  // namespace lss::players::pad_presence
