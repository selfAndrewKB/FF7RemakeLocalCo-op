#pragma once

// The seats: the further human players the mod adds (plan/seats.md). Seat 0 is player 1, the game's own player;
// seat 1 is player 2, seat 2 player 3. A seat's number is at once the scopes' player (players/scopes.h), the engine
// list's index its local player takes (the seats before it filled first) and its controller id; its pad is XInput
// user `seat`. Pure constants and names, no game.

#include "players/scopes.h"

namespace lss::players {

/// The further players built and wired: seats 1..kSeatsBuilt. The modules behind them are sized by
/// scopes::kMaxPlayers; the seats are wired one at a time (plan/seats.md step 6 wires seat 2).
inline constexpr int kSeatsBuilt = 1;
static_assert(kSeatsBuilt >= 1 && kSeatsBuilt < scopes::kMaxPlayers, "the seats are numbered within the scopes' players");

/// "player 2" for seat 1: the name every line of a seat's uses, so three players read apart in the log.
constexpr const wchar_t* SeatName(int seat) noexcept
{
    constexpr const wchar_t* kNames[scopes::kMaxPlayers] = {L"player 1", L"player 2", L"player 3", L"player 4"};
    return seat >= 0 && seat < scopes::kMaxPlayers ? kNames[seat] : L"a player";
}

/// A seat is one of the built ones.
constexpr bool SeatBuilt(int seat) noexcept
{
    return seat >= 1 && seat <= kSeatsBuilt;
}

}  // namespace lss::players
