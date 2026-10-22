// Host-side tests for the coin's choice (battle/coin_choice.h): the game's own answer without a further player, the
// only standing human, a fair pick among the standing ones, and player 1 when nobody stands. Run without the game.

#include "battle/coin_choice.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "coin_choice: all passed\n" : "coin_choice: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
}  // namespace lss::test

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++lss::test::g_failures;                                         \
        }                                                                    \
    } while (false)

namespace {

using namespace lss::battle::coin_choice;

constexpr unsigned kOne = 1u << 0, kTwo = 1u << 1, kThree = 1u << 2;

void NoFurtherPlayer()
{
    const Choice c = Choose(kOne, kOne, 7);
    EXPECT(c.player == kGamesOwn && c.why == Why::NoFurtherPlayer);
    EXPECT(Choose(kOne, 0, 0).player == kGamesOwn);
}

void TwoPlayers()
{
    EXPECT(Choose(kOne | kTwo, kOne | kTwo, 0).player == 0);
    EXPECT(Choose(kOne | kTwo, kOne | kTwo, 1).player == 1);
    EXPECT(Choose(kOne | kTwo, kOne | kTwo, 1).why == Why::Coin);
    EXPECT(Choose(kOne | kTwo, kOne | kTwo, 3).player == 1);  // reduced modulo two
    const Choice onlyTwo = Choose(kOne | kTwo, kTwo, 0);
    EXPECT(onlyTwo.player == 1 && onlyTwo.why == Why::OnlyEligible);
    const Choice onlyOne = Choose(kOne | kTwo, kOne, 1);
    EXPECT(onlyOne.player == 0 && onlyOne.why == Why::OnlyEligible);
    const Choice nobody = Choose(kOne | kTwo, 0, 1);
    EXPECT(nobody.player == 0 && nobody.why == Why::NeitherEligible);
    // Eligibility of a seat nobody holds does not count.
    EXPECT(Choose(kOne | kTwo, kOne | kThree, 1).player == 0);
}

void ThreePlayers()
{
    const unsigned all = kOne | kTwo | kThree;
    EXPECT(Choose(all, all, 0).player == 0);
    EXPECT(Choose(all, all, 1).player == 1);
    EXPECT(Choose(all, all, 2).player == 2);
    EXPECT(Choose(all, all, 5).player == 2);
    EXPECT(Choose(all, kTwo | kThree, 0).player == 1);
    EXPECT(Choose(all, kTwo | kThree, 1).player == 2);
    EXPECT(Choose(all, kThree, 0).player == 2);
    EXPECT(Choose(all, kThree, 0).why == Why::OnlyEligible);
}

void NthSetBits()
{
    EXPECT(NthSet(0b1011, 0) == 0);
    EXPECT(NthSet(0b1011, 1) == 1);
    EXPECT(NthSet(0b1011, 2) == 3);
}

}  // namespace

int main()
{
    NoFurtherPlayer();
    TwoPlayers();
    ThreePlayers();
    NthSetBits();
    return lss::test::Report();
}
