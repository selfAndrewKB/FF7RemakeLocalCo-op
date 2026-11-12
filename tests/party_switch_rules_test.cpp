// Host-side tests for the battle switch's co-op rules (battle/party_switch_rules.h). Run without the game.

#include "battle/party_switch_rules.h"

#include <cstdio>

namespace {

using namespace lss::battle::party_switch_rules;

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

/// Only a downed player's press in co-op runs the cycle as the game's forced switch does; single player keeps the gate.
void TheDownedPlayersPress()
{
    EXPECT(ForcedCycle(true, true));
    EXPECT(!ForcedCycle(true, false));   // standing: the game's two-standing gate stays
    EXPECT(!ForcedCycle(false, true));   // single player: the game's own forced switch handles a downed leader
    EXPECT(!ForcedCycle(false, false));
}

/// The own switch holds the member through the fight and lets the row decide again once the fight has been over for a
/// few ticks in a row.
void TheFightLongHold()
{
    EXPECT(OwnSwitchHolds(true, true));
    EXPECT(!OwnSwitchHolds(true, false));
    EXPECT(!OwnSwitchHolds(false, true));
    EXPECT(!OwnSwitchEnds(true, 0));
    EXPECT(!OwnSwitchEnds(true, kOwnSwitchEndTicks - 1));  // a blip of the in-battle test
    EXPECT(OwnSwitchEnds(true, kOwnSwitchEndTicks));
    EXPECT(!OwnSwitchEnds(false, kOwnSwitchEndTicks));     // no switch to end
}

}  // namespace

int main()
{
    TheDownedPlayersPress();
    TheFightLongHold();
    if (g_failures != 0)
    {
        std::printf("party_switch_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party_switch_rules: all passed\n");
    return 0;
}
