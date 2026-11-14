#pragma once

// Which party member each of players 2 and 3 plays, as a place in the game's party order (players/party_member;
// a player window's arrows, players/player_window). Pure: host-tested (tests/party_rank_rules_test.cpp).
//
// A rank is a place among the rows player 2 or 3 may play, counted from the field leader's in the formation's
// order: rank 1 the first such row, rank 2 the next. No two players hold one rank. A player joining takes the lowest
// rank no other player holds whose member is free, else the lowest no other player holds (the next party member no
// human plays); a player window's arrows step it over every rank, wrapping, and a step onto a rank another player holds
// trades ranks with that player (each keeps a character). The count of ranks is the game's party at that moment (a
// story party change moves it). When the party's members change, each player's rank moves with the character it plays,
// and a waiting player's to the character it last played once that one is back (KeepCharacters): a member joining ahead
// in the formation's order shifts the rows, never the players' characters.

#include <cstdint>
#include <initializer_list>

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

/// One player's claim when the party's members change: the character it keeps, its rank before the change, and whether it
/// plays that character now (a player playing goes before a waiting one).
struct Keeper
{
    std::uint8_t type  = 0;      // the character it plays, or last played while it waits (a type no row holds: none)
    int          rank  = 0;      // its rank before the change; the new one after KeepCharacters
    bool         plays = false;  // it holds a member now
};

/// The players' ranks after the party's members changed, so that each keeps its character: a player takes the rank whose
/// character (`rankTypes[r - 1]`, ranks 1..count) is the one it keeps, the players playing first, then those waiting; a
/// player whose character is not in the party keeps its rank if no one took it, else the lowest rank of a formation no
/// one holds (its member then decides: taken, or waited for), else its rank. No two players end on one rank when they did
/// not start on one. At most 32 keepers.
constexpr void KeepCharacters(Keeper* keepers, int n, const std::uint8_t* rankTypes, int count) noexcept
{
    const int     ranks   = RanksOf(count);
    std::uint32_t claimed = 0;
    std::uint32_t placed  = 0;  // bit i: keepers[i] has its rank
    for (const bool playing : {true, false})
    {
        for (int i = 0; i < n && i < 32; ++i)
        {
            if (keepers[i].plays != playing)
            {
                continue;
            }
            for (int rank = 1; rank <= ranks; ++rank)
            {
                if (rankTypes[rank - 1] == keepers[i].type && !Used(claimed, rank))
                {
                    keepers[i].rank = rank;
                    claimed |= Bit(rank);
                    placed |= 1u << i;
                    break;
                }
            }
        }
    }
    for (int i = 0; i < n && i < 32; ++i)
    {
        const int rank = keepers[i].rank;
        if ((placed & 1u << i) == 0 && rank >= 1 && rank <= kMaxRanks && !Used(claimed, rank))
        {
            claimed |= Bit(rank);
            placed |= 1u << i;
        }
    }
    for (int i = 0; i < n && i < 32; ++i)
    {
        if ((placed & 1u << i) == 0)
        {
            if (const int lowest = Lowest(claimed, kMaxRanks); lowest != 0)
            {
                keepers[i].rank = lowest;
                claimed |= Bit(lowest);
            }
        }
    }
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
