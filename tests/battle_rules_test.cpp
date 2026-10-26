// Host-side tests for the battle warps' Stays rule (battle/battle_warps_rules.h) and the arena warp's double click
// (battle/arena_warp_rules.h). Run without the game.

#include "battle/arena_warp_rules.h"
#include "battle/battle_warps_rules.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "battle_rules: all passed\n" : "battle_rules: %d failure(s)\n", g_failures);
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

void Stays()
{
    using namespace lss::battle::battle_warps_rules;
    static_assert(!Stays(Mode::Game, 0) && !Stays(Mode::Game, 1));            // the game's warps for everyone
    static_assert(Stays(Mode::Off, 0) && Stays(Mode::Off, 1) && Stays(Mode::Off, 2));  // nobody warped
    static_assert(!Stays(Mode::PlayersStay, 0));  // player 1's character: the game's leader logic
    static_assert(Stays(Mode::PlayersStay, 1) && Stays(Mode::PlayersStay, 2)); // the further players' stay
}

void DoubleClicks()
{
    using namespace lss::battle::arena_warp_rules;
    DoubleClick click;
    EXPECT(!click.Press(1000));            // the first press of a pair
    EXPECT(click.Press(1000 + kDoubleClickMs));  // the second, at the edge: a double click
    EXPECT(click.lastPress == 0);          // spent
    EXPECT(!click.Press(5000));            // a new pair
    EXPECT(!click.Press(5000 + kDoubleClickMs + 1));  // too late: this press starts another pair instead
    EXPECT(click.lastPress == 5000 + kDoubleClickMs + 1);
    EXPECT(click.Press(5000 + kDoubleClickMs + 100));  // within the new pair
    constexpr bool kSingle = [] { DoubleClick c; return c.Press(10); }();
    static_assert(!kSingle);
}

}  // namespace

int main()
{
    Stays();
    DoubleClicks();
    return lss::test::Report();
}
