#pragma once

// The view columns: how many views the screen shows and whose each one is, read once per tick from the engine's
// local-player list (plan/seats.md step 5; cleanup step 5, plan/code-alignment.md A1; Rebirth's views/columns.h, the
// Map taken as is). Pure, no game; host-tested (tests/columns_test.cpp).
//
// A column is one view slot of the engine's local player list, laid out by the engine: two players top and bottom
// (one window) or side by side (the span), three as the engine's three-player entry or the mod's three equal columns
// (views/column_rules.h). Column 0 is player 1's: the list's first, which no leave removes. The seats in
// (players/seats.h) follow in the order they joined (a join appends; a leave closes the list up), and the mirror
// players fill the rest (views/mirror_player.h): a mirror is a local player of the mod's that shows player 1's
// picture, so a column no player has is never black while more columns are shown than players are in. A column is a
// list index; a seat is a controller id; the two are never taken for each other. Where a column shows on screen is
// the column order's (views/column_rules.h `column-order`).
//
// Unlike Rebirth's, Remake's mirrors take the lowest free controller id (the engine routes XInput user N to id N, so
// a mirror at a seat's id would swallow its pad while in; the session's join takes the mirrors out first), so a
// mirror is not told by its id: the entry point names each place a seat, a mirror (mirror_player knows its places)
// or unknown, and the Map is built from that.

#include "players/scopes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace lss::views::columns {

/// The most columns the mod lays out: one per player (player 1 and two seats), the engine's three-player entry.
inline constexpr int kMaxColumns = players::scopes::kMaxPlayers - 1;
static_assert(kMaxColumns == 3, "player 1 and two further seats: three columns (hud/overlay_pass.h, views/column_rules.h)");

/// What fills a column that no seat does.
inline constexpr int kMirror  = -2;  // a mirror player's view
inline constexpr int kUnknown = -1;  // no view, or a local player the mod does not know

/// The engine's local player list as columns: each column's seat (or kMirror, kUnknown) and each seat's column.
struct Map
{
    int                                   count = 0;  // local players listed, one column each
    std::array<int, kMaxColumns>          seatAt{kUnknown, kUnknown, kUnknown};
    std::array<int, players::scopes::kMaxPlayers> columnOf{-1, -1, -1, -1};  // -1: the seat is not listed
    int                                   mirrors = 0;

    /// The seat whose column this is (0 player 1's), or kMirror / kUnknown.
    constexpr int SeatOf(int column) const noexcept
    {
        return column >= 0 && column < count && column < kMaxColumns ? seatAt[static_cast<std::size_t>(column)] : kUnknown;
    }

    /// The seat's column, or -1 (not listed).
    constexpr int ColumnOf(int seat) const noexcept
    {
        return seat >= 0 && seat < players::scopes::kMaxPlayers ? columnOf[static_cast<std::size_t>(seat)] : -1;
    }

    bool operator==(const Map&) const noexcept = default;
};

/// The map of a list whose places hold `seatAtPlace` (a seat 0.., kMirror or kUnknown), in list order. A list longer
/// than kMaxColumns counts only its first kMaxColumns (the engine lays out no more as columns); a seat listed twice
/// keeps its first column, the later place reading kUnknown.
constexpr Map MapOf(std::span<const int> seatAtPlace) noexcept
{
    Map map;
    map.count = static_cast<int>(std::min<std::size_t>(seatAtPlace.size(), kMaxColumns));
    for (int column = 0; column < map.count; ++column)
    {
        const auto at   = static_cast<std::size_t>(column);
        const int  seat = seatAtPlace[at];
        if (seat == kMirror)
        {
            map.seatAt[at] = kMirror;
            ++map.mirrors;
        }
        else if (seat >= 0 && seat < players::scopes::kMaxPlayers && map.columnOf[static_cast<std::size_t>(seat)] < 0)
        {
            map.seatAt[at]                                = seat;
            map.columnOf[static_cast<std::size_t>(seat)] = column;
        }
    }
    return map;
}

}  // namespace lss::views::columns
