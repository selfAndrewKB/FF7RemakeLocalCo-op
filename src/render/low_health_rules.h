#pragma once

// The low-health red edge's rules for each player's view (render/low_health.h). Pure, no game; host-tested
// (tests/low_health_rules_test.cpp).
//
// Whose danger a controller's tick counts: a seat's controller its own member only; player 1's controller player 1's
// member and every member no seat holds (an AI member low lights player 1's view, as in single player). Whose view a
// column is: the column's seat, a mirror's (which shows player 1's picture) player 1's. What a view takes: its player's
// own fade, never above the game's shared value, which carries every gate of the game's (a menu's fade-out, its return).

#include "views/columns.h"

#include <array>
#include <cstdint>

namespace lss::render::low_health_rules {

inline constexpr int          kPlayers = players::scopes::kMaxPlayers;
inline constexpr std::uint8_t kNoType  = 9;  // EPlayerType NONE: a seat that holds no member

/// The member type each player holds, by player (index 0 unused: player 1 counts every type no seat holds).
using Held = std::array<std::uint8_t, kPlayers>;

/// Whether member `type`'s HP counts for the danger of `player`'s controller (0 player 1's).
constexpr bool Counts(int player, std::uint8_t type, const Held& held) noexcept
{
    if (player > 0)
    {
        return player < kPlayers && held[static_cast<std::size_t>(player)] != kNoType && held[static_cast<std::size_t>(player)] == type;
    }
    for (int seat = 1; seat < kPlayers; ++seat)
    {
        if (held[static_cast<std::size_t>(seat)] == type)
        {
            return false;
        }
    }
    return true;
}

/// The fade's target for a player: the game's "on" intensity while their controller is in danger and the effect is
/// allowed (an event hides it through player 1's controller only), else 0.
constexpr float Target(bool inDanger, bool allowed, float on) noexcept
{
    return inDanger && allowed ? on : 0.0f;
}

/// The intensity a player's view takes: their own fade, capped by the game's.
constexpr float ViewIntensity(float own, float game) noexcept
{
    return own < game ? own : game;
}

/// The player whose view sits at `place` in the engine's list (a column), or -1 for a view no player's.
constexpr int ViewPlayer(int place, const views::columns::Map& map) noexcept
{
    if (place < 0)
    {
        return -1;
    }
    const int seat = map.SeatOf(place);
    return seat == views::columns::kMirror ? 0 : seat >= 0 && seat < kPlayers ? seat : -1;
}

}  // namespace lss::render::low_health_rules
