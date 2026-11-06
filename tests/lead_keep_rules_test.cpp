// Host-side tests for lead_keep_rules (players/lead_keep_rules.h): whether a further player keeps a member the level
// walks to its marks, and when it goes to its AI at a mark. Run without the game. The cases are the rows read in
// research/RE-holder-keeps-lead.md and RE-lead-marker.md (Ch. 4 Tifa, Ch. 8 Aerith's walk home).

#include "players/lead_keep_rules.h"

#include <cstdio>
#include <initializer_list>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "lead_keep_rules: all passed\n" : "lead_keep_rules: %d failure(s)\n", g_failures);
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

using namespace lss::players::lead_keep_rules;

// The stepless arrival radius of the rows read (GoalRadius - 15 cm): Aerith's walk rows and Tifa's 35 cm,
// MovetoMonitor 85 cm (research/RE-holder-keeps-lead.md).
constexpr float kSteplessRows[] = {35.0f, 85.0f};

void TestTheHandRadiusLeavesTheAiTheLastSteps()
{
    for (const float radius : kSteplessRows)
    {
        EXPECT(kHandRadius > radius);
    }
}

void TestAerithsRunToTheMonitorIsKeptThenHandedAtItsPoint()
{
    // scg080_SLU5B_20_Aerith_MovetoMonitor: a Lead to oblPoint080_SLU5B_020_Aerith_Monitor whose arrival the level
    // waits on (LookMonitor next); the point is a field point.
    EXPECT(Decide(true, true, true, true, false) == Verdict::Keep);
    EXPECT(Decide(true, true, true, true, true) == Verdict::AtMark);
}

void TestTifasCh4LeadIsKeptThroughout()
{
    // scg030_Tifa_Day2_2010_End: no arrival trigger; the leash alone needs her. Nothing to signal: never handed.
    EXPECT(Decide(true, true, true, false, false) == Verdict::Keep);
    EXPECT(Decide(true, true, true, false, true) == Verdict::Keep);
}

void TestAMoveWithNoFieldPointIsAGuide()
{
    // A follow or a destination named by an actor: nothing to mark.
    EXPECT(Decide(true, true, false, true, false) == Verdict::Guide);
    EXPECT(Decide(true, true, false, true, true) == Verdict::Guide);
}

void TestTheGuideSwitchBringsBackTheGuide()
{
    EXPECT(Decide(true, false, true, true, false) == Verdict::Guide);
    EXPECT(Decide(true, false, true, false, false) == Verdict::Guide);
}

void TestStagingStaysThePlayers()
{
    // A Lead nothing needs (Barret's walk-up to the console, Ch. 7): no marker, no hand.
    for (const bool keeps : {false, true})
    {
        for (const bool reached : {false, true})
        {
            EXPECT(Decide(false, keeps, true, true, reached) == Verdict::Staging);
        }
    }
}

void TestARunIntoItsOwnBoxIsAGuide()
{
    // Tifa's Ch. 5 dash to the fence: nothing waits on its arrival, but it moves on at her fence box, before the talk's timed
    // "head for the stairs". A guide whatever else holds, the keep switch included.
    for (const bool needed : {false, true})
    {
        for (const bool keeps : {false, true})
        {
            EXPECT(Decide(needed, keeps, true, true, false, true) == Verdict::Guide);
            EXPECT(Decide(needed, keeps, true, true, true, true) == Verdict::Guide);
        }
    }
    EXPECT(Decide(false, true, true, true, false, false) == Verdict::Staging);
}

void TestOnlyTheGuideAndTheMarkHand()
{
    EXPECT(ToAi(Verdict::Guide));
    EXPECT(ToAi(Verdict::AtMark));
    EXPECT(!ToAi(Verdict::Keep));
    EXPECT(!ToAi(Verdict::Staging));
}

void TestReachedIsTwoDimensionalAndInclusive()
{
    EXPECT(Reached(0.0f, 0.0f));
    EXPECT(Reached(200.0f, 0.0f));
    EXPECT(Reached(120.0f, 160.0f));  // 200 cm
    EXPECT(!Reached(120.0f, 161.0f));
    EXPECT(!Reached(0.0f, -201.0f));
    // Box 0040 lies 9.6 m past stop000 (research/RE-holder-keeps-lead.md): far outside the circle.
    EXPECT(!Reached(960.0f, 0.0f));
}

}  // namespace

int main()
{
    TestTheHandRadiusLeavesTheAiTheLastSteps();
    TestAerithsRunToTheMonitorIsKeptThenHandedAtItsPoint();
    TestTifasCh4LeadIsKeptThroughout();
    TestAMoveWithNoFieldPointIsAGuide();
    TestTheGuideSwitchBringsBackTheGuide();
    TestStagingStaysThePlayers();
    TestARunIntoItsOwnBoxIsAGuide();
    TestOnlyTheGuideAndTheMarkHand();
    TestReachedIsTwoDimensionalAndInclusive();
    return lss::test::Report();
}
