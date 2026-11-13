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

}  // namespace

int main()
{
    TheNextFreeMember();
    AJoinTakesAFreeMember();
    TheArrows();
    AShrunkParty();
    if (g_failures != 0)
    {
        std::printf("party_rank_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party_rank_rules: all passed\n");
    return 0;
}
