// Host-side tests for stuck_spot_rules (world/stuck_spot_rules.h): a spot's passes, which waiting states a pass holds,
// and the verdict on sending its ends. Run without the game. The names are Sector 5's hideout squeeze
// (obl080_SLU5B_20_Crack_SecretBaseEntrance and obj080_10_AimCameraManager3, read live from a save that held it), the
// cave's two passes (120-UNDG7) and the ladders that share their signals, as the research read them
// (research/RE-stuck-spot-repair.md); the numbers stand in for the engine's name bits.

#include "world/stuck_spot_rules.h"

#include <cstdio>
#include <vector>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "stuck_spot_rules: all passed\n" : "stuck_spot_rules: %d failure(s)\n", g_failures);
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

using namespace lss::world::stuck_spot_rules;

// trg080_SLU5B_20_Timing_Start_SecretBaseSukima / _End_SecretBaseSukima
constexpr Name kHideoutStart = 101, kHideoutEnd = 102;
// trg120_HoleJumpR_start / _end, trg120_ClimbL_start / _end
constexpr Name kHoleStart = 201, kHoleEnd = 202, kClimbStart = 203, kClimbEnd = 204;
// trg080_SLU5B_10_OnGoUp_KaribaLadder / OnGoDown, trg080_10_FieldAction_KaribaGmk_Clear
constexpr Name kLadderUp = 301, kLadderDown = 302, kLadderClear = 303;
// another spot's start, a story trigger
constexpr Name kOtherSpotStart = 401, kStory = 402;

/// The hideout squeeze: a default pass only.
constexpr Signals kHideout{.defaultStart = kHideoutStart, .defaultEnd = kHideoutEnd};

/// The camera manager's resting state, as its row lists it: four starts, the hideout's among them.
const std::vector<Step> kManagerInit{{301, L"scg080_10_AimCameraManager3_KaribaUnteiSub01"},
                                     {302, L"scg080_10_AimCameraManager3_KaribaUnteiSub02"},
                                     {303, L"scg080_10_AimCameraManager3_KaribaGmk"},
                                     {kHideoutStart, L"scg080_20_AimCameraManager3_EnterSecretBaseRail"}};

void TestADefaultOnlySpotHasOnePass()
{
    const std::vector<Pass> passes = Passes(kHideout);
    EXPECT(passes.size() == 1);
    EXPECT(passes[0].starts[0] == kHideoutStart && passes[0].starts[1] == 0);
    EXPECT(passes[0].ends[0] == 0 && passes[0].ends[1] == kHideoutEnd);
}

void TestTwoSidedSpotHasTwoPassesEndingSideFirst()
{
    const Signals          cave{.leftStart = kClimbStart, .leftEnd = kClimbEnd, .rightStart = kHoleStart, .rightEnd = kHoleEnd};
    const std::vector<Pass> passes = Passes(cave);
    EXPECT(passes.size() == 2);
    EXPECT(passes[0].left && Lists(passes[0].starts, kClimbStart) && passes[0].ends[0] == kClimbEnd);
    EXPECT(!passes[1].left && Lists(passes[1].starts, kHoleStart) && passes[1].ends[0] == kHoleEnd);
}

void TestALadderWithAClearDefaultSendsItsDefaultEndLast()
{
    const Signals          ladder{.defaultStart = kLadderClear, .leftStart = kLadderUp, .leftEnd = kLadderDown};
    const std::vector<Pass> passes = Passes(ladder);
    EXPECT(passes.size() == 1);  // the right side names nothing of its own: the same default start, no end
    EXPECT(passes[0].left && Lists(passes[0].starts, kLadderClear) && Lists(passes[0].starts, kLadderUp));
    EXPECT(passes[0].ends[0] == kLadderDown && passes[0].ends[1] == 0);
}

void TestEndsOnlyOrStartsOnlyCannotHoldAnything()
{
    EXPECT(Passes(Signals{.leftEnd = 501, .rightEnd = 502}).empty());   // a trap that only reports in and out
    EXPECT(Passes(Signals{.defaultStart = 503}).empty());               // a jukebox, a door that only starts
}

void TestTheHideoutRailIsHeldByTheHideoutPass()
{
    const Pass pass = Passes(kHideout)[0];
    EXPECT(HeldBy(pass, L"scg080_20_AimCameraManager3_EnterSecretBaseRail", kManagerInit));
    EXPECT(HeldBy(pass, L"SCG080_20_AIMCAMERAMANAGER3_ENTERSECRETBASERAIL", kManagerInit));  // names compare without case
}

void TestAStateAnotherSpotStartedIsNotHeld()
{
    // A camera meant to stay on between two spots: started by one spot, ended by this one.
    const Pass              pass = Passes(kHideout)[0];
    const std::vector<Step> back{{kOtherSpotStart, L"scg_Camera_Between"}};
    EXPECT(!HeldBy(pass, L"scg_Camera_Between", back));
}

void TestAOneWayMoveIsNotHeld()
{
    // The end would move it, but nothing of this pass leads back: a state the story or a box put there.
    const Pass              pass = Passes(kHideout)[0];
    const std::vector<Step> back{{kStory, L"scg_Box_on"}, {kHideoutStart, L"scg_Box_Other"}};
    EXPECT(!HeldBy(pass, L"scg_Box_on", back));
    EXPECT(!HeldBy(pass, L"scg_Box_on", {}));  // the state it returns to has no row
}

EndCheck Clean(Name trigger, int moves, int held)
{
    return EndCheck{.trigger = trigger, .moves = moves, .held = held, .scope = 1};
}

void TestTheHideoutPassSendsItsEnd()
{
    const std::vector<EndCheck> ends{Clean(kHideoutEnd, 1, 1)};
    const Plan                  plan = Judge(false, ends);
    EXPECT(plan.held && plan.refusal.empty());
    EXPECT(plan.send.size() == 1 && plan.send[0] == kHideoutEnd);
}

void TestTheCavePassSendsBothEndsSideFirst()
{
    // The right pass: its own end moves the camera manager and the box, the default end moves nothing here.
    const std::vector<EndCheck> ends{Clean(kHoleEnd, 2, 2), Clean(kHideoutEnd, 1, 1)};
    const Plan                  plan = Judge(false, ends);
    EXPECT(plan.send.size() == 2 && plan.send[0] == kHoleEnd && plan.send[1] == kHideoutEnd);
}

void TestAnEndThatMovesNothingIsNotSent()
{
    const std::vector<EndCheck> ends{Clean(kClimbEnd, 1, 1), Clean(kHideoutEnd, 0, 0)};
    const Plan                  plan = Judge(false, ends);
    EXPECT(plan.send.size() == 1 && plan.send[0] == kClimbEnd);
}

void TestNothingHeldIsNoVerdict()
{
    const std::vector<EndCheck> ends{Clean(kHideoutEnd, 3, 0)};
    const Plan                  plan = Judge(true, ends);
    EXPECT(!plan.held && plan.send.empty() && plan.refusal.empty());
}

void TestEachRefusal()
{
    EXPECT(Judge(true, std::vector<EndCheck>{Clean(kHideoutEnd, 1, 1)}).refusal == L"a one-shot spot");

    EndCheck saves = Clean(kHideoutEnd, 1, 1);
    saves.rowRefusal = L"its row writes a save record when sent (save step 1)";
    EXPECT(Judge(false, std::vector<EndCheck>{saves}).refusal == saves.rowRefusal);

    EndCheck listed = Clean(kHideoutEnd, 1, 1);
    listed.scope    = 3;
    EXPECT(Judge(false, std::vector<EndCheck>{listed}).send.empty());

    // The end would also move a state this pass did not leave on (a sub-quest's, a story manager's).
    const Plan more = Judge(false, std::vector<EndCheck>{Clean(kHideoutEnd, 2, 1)});
    EXPECT(more.send.empty() && more.refusal.find(L"1 state(s)") != std::wstring::npos);

    EndCheck story  = Clean(kHideoutEnd, 1, 1);
    story.storyActs = L"finishing the held state's act would set a story flag";
    EXPECT(Judge(false, std::vector<EndCheck>{story}).refusal == story.storyActs);

    EndCheck chained = Clean(kHideoutEnd, 1, 1);
    chained.chained  = true;
    EXPECT(Judge(false, std::vector<EndCheck>{chained}).send.empty());
}

void TestAReturnedStateThatWaitsOnTheEndsAgainIsSeen()
{
    const Pass pass = Passes(Signals{.defaultStart = kHoleStart, .defaultEnd = kHideoutEnd, .rightStart = kHoleStart,
                                     .rightEnd = kHoleEnd})[0];
    EXPECT(!WaitsOnEnds(pass, kManagerInit));  // the hideout manager's rest waits on starts only
    const std::vector<Step> back{{kHoleStart, L"scg_Now"}, {kHideoutEnd, L"scg_Further"}};
    EXPECT(WaitsOnEnds(pass, back));
}

void TestARefusedSecondEndSendsNeither()
{
    EndCheck second  = Clean(kHideoutEnd, 1, 1);
    second.rowRefusal = L"its row sets objects directly, whatever their state";
    const Plan plan = Judge(false, std::vector<EndCheck>{Clean(kHoleEnd, 2, 2), second});
    EXPECT(plan.held && plan.send.empty() && plan.refusal == second.rowRefusal);
}

}  // namespace

int main()
{
    TestADefaultOnlySpotHasOnePass();
    TestTwoSidedSpotHasTwoPassesEndingSideFirst();
    TestALadderWithAClearDefaultSendsItsDefaultEndLast();
    TestEndsOnlyOrStartsOnlyCannotHoldAnything();
    TestTheHideoutRailIsHeldByTheHideoutPass();
    TestAStateAnotherSpotStartedIsNotHeld();
    TestAOneWayMoveIsNotHeld();
    TestTheHideoutPassSendsItsEnd();
    TestTheCavePassSendsBothEndsSideFirst();
    TestAnEndThatMovesNothingIsNotSent();
    TestNothingHeldIsNoVerdict();
    TestEachRefusal();
    TestAReturnedStateThatWaitsOnTheEndsAgainIsSeen();
    TestARefusedSecondEndSendsNeither();
    return lss::test::Report();
}
