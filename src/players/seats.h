#pragma once

// The seats: the further human players the mod adds (plan/seats.md). Seat 0 is player 1, the game's own player;
// seat 1 is player 2, seat 2 player 3. A seat's number is at once the scopes' player (players/scopes.h), its
// controller id and its row of the party order after the field leader's; its pad is whichever physical pad the pad
// table puts on slot `seat` (players/pad_table). Its column is its local player's index in the engine's list, which a
// join appends to: player 2 and player 3 in that order take columns 1 and 2, but with player 2 out player 3 takes
// column 1 (the HUD asks overlay_pass::ColumnOf, never assumes the seat). Where a column shows on screen is the column
// order's (views/column_rules.h, `column-order`), by player number whatever its column: by default players 1, 2, 3
// left to right; `1 3 2` puts player 2's on the right.
// The code counts columns from 0 (player 1's); the log lines count them from 1 ("column 2" is the first further one).
// Pure constants and names, no game.

#include "players/scopes.h"

namespace lss::players {

/// The further players built and wired: seats 1..kSeatsBuilt (plan/seats.md step 6 wired seat 2, player 3). The
/// modules behind them are sized by scopes::kMaxPlayers.
inline constexpr int kSeatsBuilt = 2;
static_assert(kSeatsBuilt >= 1 && kSeatsBuilt < scopes::kMaxPlayers, "the seats are numbered within the scopes' players");

/// Whether a further player holds a member (co-op, as the battle levers read it). A plain pointer read per seat, so
/// any thread may ask (the seats are bound on the game thread).
inline bool AnyFurtherMember() noexcept
{
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (scopes::Member(seat) != nullptr)
        {
            return true;
        }
    }
    return false;
}

/// "player 2" for seat 1: the name every line of a seat's uses, so three players read apart in the log.
constexpr const wchar_t* SeatName(int seat) noexcept
{
    constexpr const wchar_t* kNames[scopes::kMaxPlayers] = {L"player 1", L"player 2", L"player 3", L"player 4"};
    return seat >= 0 && seat < scopes::kMaxPlayers ? kNames[seat] : L"a player";
}

/// The keys that bring the seat's player in and take it out (mod.cpp reads them): Ctrl+F7/F8 for player 2,
/// Ctrl+Shift+F7/F8 for player 3. Named in the seat's log lines.
constexpr const wchar_t* JoinKey(int seat) noexcept
{
    return seat == 1 ? L"Ctrl+F7" : seat == 2 ? L"Ctrl+Shift+F7" : L"the console's join";
}
constexpr const wchar_t* LeaveKey(int seat) noexcept
{
    return seat == 1 ? L"Ctrl+F8" : seat == 2 ? L"Ctrl+Shift+F8" : L"the console's leave";
}

/// A seat is one of the built ones.
constexpr bool SeatBuilt(int seat) noexcept
{
    return seat >= 1 && seat <= kSeatsBuilt;
}

}  // namespace lss::players
