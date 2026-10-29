// Host-side tests for stall_rules (battle/stall_rules.h): when a ready enemy that has not acted counts as stalled, and
// when the game may pick again for it. Run without the game.
//
// The case from 2026-10-29 10:51: a MonoDrive (light interval 3.3 s) at full ATB aimed at player 3 at 28 m, player 2
// at 0.7 m; its heavy attack needs the target within 20 m, so it waited 95 s.

#include "battle/stall_rules.h"

#include <cstdio>
#include <limits>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "stall_rules: all passed\n" : "stall_rules: %d failure(s)\n", g_failures);
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

using namespace lss::battle::stall_rules;

Facts Stalled()
{
    Facts f;
    f.read           = true;
    f.hate           = true;
    f.targetSeat     = 2;
    f.targetStanding = true;
    f.otherStanding  = true;
    return f;
}

void TestThresholdFollowsTheEnemysClock()
{
    EXPECT(ThresholdMs(3.3f) == 6600);   // the MonoDrive
    EXPECT(ThresholdMs(5.0f) == 10000);  // most enemies
    EXPECT(ThresholdMs(1.0f) == kFloorMs);
    EXPECT(ThresholdMs(0.0f) == kFloorMs);
    EXPECT(ThresholdMs(-2.0f) == kFloorMs);
    EXPECT(ThresholdMs(std::numeric_limits<float>::quiet_NaN()) == kFloorMs);
    EXPECT(ThresholdMs(std::numeric_limits<float>::infinity()) == kFloorMs);
    EXPECT(ThresholdMs(500.0f) == kCeilingMs);
}

void TestDueAfterTheWaitAndOncePerThreshold()
{
    EXPECT(!Due(10000, 0, 0, 6600));         // not ready
    EXPECT(!Due(10000, 5000, 0, 6600));      // ready 5 s: a natural wait
    EXPECT(Due(11600, 5000, 0, 6600));       // ready 6.6 s
    EXPECT(!Due(15000, 5000, 11600, 6600));  // just decided: the next one a threshold later
    EXPECT(Due(18200, 5000, 11600, 6600));
}

void TestTheMonoDriveCasePicksAgain()
{
    EXPECT(Judge(Stalled()) == Skip::None);
}

void TestEachEdgeLeavesItToTheGame()
{
    Facts f = Stalled();
    f.read  = false;
    EXPECT(Judge(f) == Skip::Unread);
    f      = Stalled();
    f.hate = false;
    EXPECT(Judge(f) == Skip::NotHate);
    f        = Stalled();
    f.locked = true;
    EXPECT(Judge(f) == Skip::Locked);
    f            = Stalled();
    f.forcedList = true;
    EXPECT(Judge(f) == Skip::ForcedList);
    f         = Stalled();
    f.blocked = true;
    EXPECT(Judge(f) == Skip::Blocked);
    f               = Stalled();
    f.givesUpItself = true;
    EXPECT(Judge(f) == Skip::GivesUpItself);
    f            = Stalled();
    f.targetSeat = -1;
    EXPECT(Judge(f) == Skip::NotAHuman);
    f                = Stalled();
    f.targetStanding = false;
    EXPECT(Judge(f) == Skip::TargetDown);
    f               = Stalled();
    f.otherStanding = false;
    EXPECT(Judge(f) == Skip::Alone);
}

void TestPlayerOnesTargetCountsToo()
{
    Facts f      = Stalled();
    f.targetSeat = 0;
    EXPECT(Judge(f) == Skip::None);
}

void TestABossIsNeverTouchedWhateverElseHolds()
{
    Facts f   = Stalled();
    f.hate    = false;
    f.locked  = true;
    f.blocked = true;
    EXPECT(Judge(f) == Skip::NotHate);
}

}  // namespace

int main()
{
    TestThresholdFollowsTheEnemysClock();
    TestDueAfterTheWaitAndOncePerThreshold();
    TestTheMonoDriveCasePicksAgain();
    TestEachEdgeLeavesItToTheGame();
    TestPlayerOnesTargetCountsToo();
    TestABossIsNeverTouchedWhateverElseHolds();
    return lss::test::Report();
}
