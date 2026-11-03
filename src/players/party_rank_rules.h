#pragma once

// Which party member each further player plays, as a place in the game's party order (players/party_member; the
// a player window's arrows, players/player_window). Pure: host-tested (tests/party_rank_rules_test.cpp).
//
// A rank is a place among the rows a further player may play, counted from the field leader's in the formation's
// order: rank 1 the first such row, rank 2 the next. No two players hold one rank. A player joining takes the lowest
// rank no other player holds (the next party member no human plays); a player window's arrows step it over the ranks no
// other player holds, wrapping. The count of ranks is the game's party at that moment (a story party change moves it).

#include <cstdint>

namespace lss::players::party_rank_rules {

/// The most rows past player 1's: a formation holds three members.
inline constexpr int kMaxRanks = 2;

constexpr bool Used(std::uint32_t used, int rank) noexcept
{
    return rank >= 1 && rank < 32 && (used & (1u << rank)) != 0;
}

/// The lowest rank of 1..count not in `used` (bit r: rank r another player's), or 0 with none.
constexpr int Lowest(std::uint32_t used, int count) noexcept
{
    for (int rank = 1; rank <= count && rank <= kMaxRanks; ++rank)
    {
        if (!Used(used, rank))
        {
            return rank;
        }
    }
    return 0;
}

/// The next rank from `current` in `dir` (+1 right, -1 left), wrapping over 1..count and skipping `used`; `current`
/// when no other rank is free. A `current` outside 1..count (the party shrank) steps to the lowest free one.
constexpr int Step(int current, int dir, std::uint32_t used, int count) noexcept
{
    const int ranks = count < kMaxRanks ? count : kMaxRanks;
    if (ranks < 1)
    {
        return current;
    }
    if (current < 1 || current > ranks)
    {
        const int lowest = Lowest(used, ranks);
        return lowest != 0 ? lowest : current;
    }
    const int way = dir >= 0 ? 1 : ranks - 1;
    for (int k = 1; k < ranks; ++k)
    {
        const int rank = (current - 1 + way * k) % ranks + 1;
        if (!Used(used, rank))
        {
            return rank;
        }
    }
    return current;
}

}  // namespace lss::players::party_rank_rules
