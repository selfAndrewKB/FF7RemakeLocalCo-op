// Host-side tests for the further players' ranks in the party order (players/party_rank_rules.h). Run without the game.

#include "players/party_rank_rules.h"

#include <cstdio>

namespace {

using namespace lss::players::party_rank_rules;

int g_failures = 0;

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

/// A joiner takes the next member no other player plays: player 3 with player 2 out takes the first row past player 1's.
void TheNextFreeMember()
{
    EXPECT(Lowest(0, 2) == 1);
    EXPECT(Lowest(Bit(1), 2) == 2);
    EXPECT(Lowest(Bit(2), 2) == 1);
    EXPECT(Lowest(Bit(1) | Bit(2), 2) == 0);  // every member past player 1's is played
    EXPECT(Lowest(Bit(1), 1) == 0);           // a party of two, its second member played
    EXPECT(Lowest(0, 0) == 0);                // player 1 alone in the party
    EXPECT(Lowest(0, 5) == 1 && Lowest(Bit(1) | Bit(2), 5) == 0);  // never past a formation's rows
}

/// A joiner takes the first free member no other player plays; with none free, the next one, and waits for it.
void AJoinTakesAFreeMember()
{
    EXPECT(JoiningRank(0, Bit(2), 2) == 2);           // rank 1's member busy in a story moment: rank 2's is taken
    EXPECT(JoiningRank(0, Bit(1) | Bit(2), 2) == 1);  // both free: the lowest
    EXPECT(JoiningRank(0, 0, 2) == 1);                // none free: the lowest, waited on
    EXPECT(JoiningRank(Bit(2), Bit(2), 2) == 1);      // the free one is another player's: the lowest left, waited on
    EXPECT(JoiningRank(Bit(1), Bit(2), 2) == 2);
    EXPECT(JoiningRank(Bit(1) | Bit(2), Bit(1), 2) == 0);  // every rank another player's
    EXPECT(JoiningRank(0, Bit(2), 1) == 1);           // a party of two: rank 2 is not the party's
    EXPECT(JoiningRank(0, 0, 0) == 1);                // no party read: the lowest, as before
}

/// The arrows step over every rank, wrapping; a rank another player holds is stepped onto too (the session trades).
void TheArrows()
{
    EXPECT(Step(1, 1, 0, 2) == 2);
    EXPECT(Step(2, 1, 0, 2) == 1);   // wraps
    EXPECT(Step(1, -1, 0, 2) == 2);  // wraps left
    EXPECT(Step(2, -1, 0, 2) == 1);
    EXPECT(Step(1, 1, Bit(2), 2) == 2);   // the other member is player 3's: the step lands on it, the players trade
    EXPECT(Step(2, -1, Bit(1), 2) == 1);  // the same leftwards
    EXPECT(Step(1, 1, Bit(1) | Bit(2), 2) == 2);  // in range the held ranks are not read
    EXPECT(Step(1, 1, 0, 1) == 1);        // one member past player 1's: no step
    EXPECT(Step(1, 1, 0, 0) == 1);        // no party read
}

/// A rank the party no longer has (a story change shrank it) steps to the lowest free one.
void AShrunkParty()
{
    EXPECT(Step(2, 1, 0, 1) == 1);
    EXPECT(Step(2, -1, Bit(1), 1) == 2);  // that one is another player's: unchanged, the member test then waits
    EXPECT(Step(3, 1, Bit(1) | Bit(2), 2) == 3);  // every rank another player's: stays
    EXPECT(Step(0, 1, 0, 2) == 1);
}

constexpr std::uint8_t kBarret = 1, kTifa = 2, kAerith = 3, kNone = 9;

/// The party's members change: each player keeps the character it plays, or takes back the one it last played.
void KeepingCharacters()
{
    // Cloud alone; player 2 (last Barret) and player 3 (last Tifa) wait on ranks 1 and 2. Tifa joins: player 3 takes her.
    Keeper alone[2] = {{kBarret, 1, false}, {kTifa, 2, false}};
    const std::uint8_t tifaOnly[2] = {kTifa, kNone};
    KeepCharacters(alone, 2, tifaOnly, 1);
    EXPECT(alone[0].rank == 2 && alone[1].rank == 1);
    // Barret joins ahead of her in the formation: player 3 keeps Tifa, now rank 2; player 2 takes Barret back.
    Keeper joined[2] = {{kBarret, 2, false}, {kTifa, 1, true}};
    const std::uint8_t both[2] = {kBarret, kTifa};
    KeepCharacters(joined, 2, both, 2);
    EXPECT(joined[0].rank == 1 && joined[1].rank == 2);
    // Player 2 plays Tifa, player 3 has no last character: Barret joins, player 2 keeps Tifa and player 3 gets Barret's rank.
    Keeper fresh[2] = {{kTifa, 1, true}, {kNone, 2, false}};
    KeepCharacters(fresh, 2, both, 2);
    EXPECT(fresh[0].rank == 2 && fresh[1].rank == 1);
    // Nothing moved: the ranks stay.
    Keeper same[2] = {{kBarret, 1, true}, {kTifa, 2, true}};
    KeepCharacters(same, 2, both, 2);
    EXPECT(same[0].rank == 1 && same[1].rank == 2);
    // A player playing goes before a waiting one that last played the same character.
    Keeper contest[2] = {{kTifa, 2, false}, {kTifa, 1, true}};
    KeepCharacters(contest, 2, both, 2);
    EXPECT(contest[1].rank == 2 && contest[0].rank == 1);
    // Barret leaves: player 3 keeps Tifa (now rank 1); player 2, whose character is gone, waits on rank 2.
    Keeper left[2] = {{kBarret, 1, false}, {kTifa, 2, true}};
    KeepCharacters(left, 2, tifaOnly, 1);
    EXPECT(left[1].rank == 1 && left[0].rank == 2);
    // A party with no row past player 1's: nothing matches; distinct ranks stay.
    Keeper solo[2] = {{kTifa, 1, false}, {kAerith, 2, false}};
    KeepCharacters(solo, 2, tifaOnly, 0);
    EXPECT(solo[0].rank == 1 && solo[1].rank == 2);
    // One player.
    Keeper one[1] = {{kTifa, 1, true}};
    KeepCharacters(one, 1, both, 2);
    EXPECT(one[0].rank == 2);
}

}  // namespace

int main()
{
    TheNextFreeMember();
    AJoinTakesAFreeMember();
    TheArrows();
    AShrunkParty();
    KeepingCharacters();
    if (g_failures != 0)
    {
        std::printf("party_rank_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party_rank_rules: all passed\n");
    return 0;
}
