#pragma once

// Which party member each further player plays, as a place in the game's party order (players/party_member; the
// a player window's arrows, players/player_window). Pure: host-tested (tests/party_rank_rules_test.cpp).
//
// A rank is a place among the rows a further player may play, counted from the field leader's in the formation's
// order: rank 1 the first such row, rank 2 the next. No two players hold one rank. A player joining takes the lowest
// rank no other player holds whose member is free, else the lowest no other player holds (the next party member no
// human plays); a player window's arrows step it over every rank, wrapping, and a step onto a rank another player holds
// trades ranks with that player (each keeps a character). The count of ranks is the game's party at that moment (a
// story party change moves it).

#include <cstdint>

namespace lss::players::party_rank_rules {

/// The most rows past player 1's: a formation holds three members.
inline constexpr int kMaxRanks = 2;

/// Rank `rank`'s bit in a set of ranks; 0 for a rank the set cannot hold.
constexpr std::uint32_t Bit(int rank) noexcept
{
    return rank >= 1 && rank < 32 ? 1u << rank : 0u;
}

constexpr bool Used(std::uint32_t used, int rank) noexcept
{
    return (used & Bit(rank)) != 0;
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

/// The ranks a party of `count` free rows gives: never more than a formation's rows past player 1's.
constexpr int RanksOf(int count) noexcept
{
    return count < kMaxRanks ? count : kMaxRanks;
}

/// A joining player's rank: the lowest of 1..count not in `used` whose member can be taken now (bit r of `freeRanks`),
/// else the lowest not in `used` of every row a formation holds (the join then waits for that member), or 0 with none:
/// a member that cannot be taken now never holds a player out while another can.
constexpr int JoiningRank(std::uint32_t used, std::uint32_t freeRanks, int count) noexcept
{
    const int ranks = RanksOf(count);
    for (int rank = 1; rank <= ranks; ++rank)
    {
        if (!Used(used, rank) && Used(freeRanks, rank))
        {
            return rank;
        }
    }
    return Lowest(used, kMaxRanks);
}

/// The next rank from `current` in `dir` (+1 right, -1 left), wrapping over 1..count, another player's ranks included
/// (the step then trades ranks with that player; `used` is not read in range); `current` when the party has no other
/// rank. A `current` outside 1..count (the party shrank) steps to the lowest rank not in `used` (bit r: rank r another
/// player's), or stays.
constexpr int Step(int current, int dir, std::uint32_t used, int count) noexcept
{
    const int ranks = RanksOf(count);
    if (ranks < 1)
    {
        return current;
    }
    if (current < 1 || current > ranks)
    {
        const int lowest = Lowest(used, ranks);
        return lowest != 0 ? lowest : current;
    }
    if (ranks < 2)
    {
        return current;
    }
    const int way = dir >= 0 ? 1 : ranks - 1;
    return (current - 1 + way) % ranks + 1;
}

}  // namespace lss::players::party_rank_rules
