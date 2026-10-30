// Host-side tests for story_box_rules (world/story_box_rules.h): which story-box entries of a held member wait, when a
// waiting one fires, and the reach test against a placed box. Run without the game. The box is TNNL4's 0200 (Ch. 5
// tunnel, research/.ida-mcp/tifa-barret/data_tnnl4_rows.txt: centre (18.1, -360.3) m, half-size 10 x 2 m).

#include "world/story_box_rules.h"

#include <cmath>
#include <limits>
#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "story_box_rules: all passed\n" : "story_box_rules: %d failure(s)\n", g_failures);
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

using namespace lss::world::story_box_rules;

constexpr Entry kTifaWalksIntoHerOwnBox{.heldByPlayer = true, .levelSentIt = false, .groupSet = true, .groupNamesLeader = false};

void TestAMemberOnlyBoxWaitsForAHeldMember()
{
    EXPECT(HoldBack(kTifaWalksIntoHerOwnBox));
}

void TestEverythingElseIsTheGames()
{
    Entry onAi = kTifaWalksIntoHerOwnBox;
    onAi.heldByPlayer = false;
    EXPECT(!HoldBack(onAi));  // an AI ally fires its box as in single player

    Entry sent = kTifaWalksIntoHerOwnBox;
    sent.levelSentIt = true;
    EXPECT(!HoldBack(sent));  // the level sent it: the box is the move's own

    Entry party = kTifaWalksIntoHerOwnBox;
    party.groupNamesLeader = true;
    EXPECT(!HoldBack(party));  // agpCmn_Party: turn-backs and scene starts, as today

    Entry groupless = kTifaWalksIntoHerOwnBox;
    groupless.groupSet = false;
    EXPECT(!HoldBack(groupless));
}

constexpr Waiting kStillWaiting{.boxGone = false, .seated = true, .handedToAi = false, .inCinema = false, .levelMoves = false,
                                .playerOneReached = false, .awaitsRun = false, .ranIn = false, .runOver = false};

void TestAWaitingEntryWaits()
{
    EXPECT(Next(kStillWaiting) == Step::Wait);
}

void TestEachReleaseReplays()
{
    Waiting w = kStillWaiting;
    w.handedToAi = true;
    EXPECT(Next(w) == Step::Replay);  // the level's scripted move, a guide, a scene's hand

    w = kStillWaiting;
    w.seated = false;
    EXPECT(Next(w) == Step::Replay);  // its player left: back to its AI, it would follow in

    w = kStillWaiting;
    w.levelMoves = true;
    EXPECT(Next(w) == Step::Replay);

    w = kStillWaiting;
    w.playerOneReached = true;
    EXPECT(Next(w) == Step::Replay);  // where a following companion would walk in
}

void TestNothingFiresInACutscene()
{
    Waiting w = kStillWaiting;
    w.handedToAi = true;
    w.inCinema = true;
    EXPECT(Next(w) == Step::Wait);
}

void TestAGoneBoxIsLostNotReplayed()
{
    Waiting w = kStillWaiting;
    w.boxGone = true;
    w.playerOneReached = true;
    EXPECT(Next(w) == Step::Lost);
}

// TNNL4's box 0200, yaw-free here; centimetres.
constexpr Box kBox0200{.rotation = {0.0f, 0.0f, 0.0f, 1.0f}, .at = {1810.0f, -36030.0f, 0.0f}, .scale = {1.0f, 1.0f, 1.0f},
                       .extent = {1000.0f, 200.0f, 200.0f}};

void TestInsideAndMarginReach()
{
    const float centre[3] = {1810.0f, -36030.0f, 0.0f};
    EXPECT(Reaches(kBox0200, centre));
    const float justOutside[3] = {1810.0f, -36030.0f + 200.0f + 299.0f, 0.0f};  // 2.99 m past the long side
    EXPECT(Reaches(kBox0200, justOutside));
    const float farOutside[3] = {1810.0f, -36030.0f + 200.0f + 301.0f, 0.0f};
    EXPECT(!Reaches(kBox0200, farOutside));
    const float alongTheLongSide[3] = {1810.0f + 1250.0f, -36030.0f, 0.0f};  // 12.5 m along x: inside 10 m + 3 m
    EXPECT(Reaches(kBox0200, alongTheLongSide));
    const float aFloorAbove[3] = {1810.0f, -36030.0f, 401.0f};
    EXPECT(!Reaches(kBox0200, aFloorAbove));
}

void TestTheScaleCounts()
{
    Box scaled = kBox0200;
    scaled.extent[1] = 100.0f;
    scaled.scale[1]  = 2.0f;  // the same 2 m half-size as above
    const float justOutside[3] = {1810.0f, -36030.0f + 200.0f + 299.0f, 0.0f};
    EXPECT(Reaches(scaled, justOutside));
}

void TestTheRotationCounts()
{
    // A quarter turn about z (yaw 90): the box's long side now runs along world y.
    Box turned = kBox0200;
    const float half = std::sqrt(0.5f);
    turned.rotation[2] = half;
    turned.rotation[3] = half;
    const float alongWorldY[3] = {1810.0f, -36030.0f + 1250.0f, 0.0f};
    EXPECT(Reaches(turned, alongWorldY));
    const float alongWorldX[3] = {1810.0f + 1250.0f, -36030.0f, 0.0f};
    EXPECT(!Reaches(turned, alongWorldX));
}

void TestANanNeverReaches()
{
    const float nan[3] = {std::nanf(""), -36030.0f, 0.0f};
    EXPECT(!Reaches(kBox0200, nan));
}

void TestABroughtBackMemberWaitsForItsOwnRun()
{
    // Brought beside player 1 and handed to its AI for the run: neither the hand nor the level's move fires the box.
    Waiting w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = true;
    w.levelMoves = true;
    EXPECT(Next(w) == Step::Wait);
    // Its AI runs into it: the game's own enter ran.
    w.ranIn = true;
    EXPECT(Next(w) == Step::Taken);
    // Seen at the game's own enter, whoever holds it now: never a second enter.
    w.handedToAi = false;
    EXPECT(Next(w) == Step::Taken);
}

void TestABroughtBackMemberIsNeverLost()
{
    Waiting w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = true;
    w.playerOneReached = true;
    EXPECT(Next(w) == Step::Replay);
    w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = false;  // back with its player: the run's setting is over
    EXPECT(Next(w) == Step::Replay);
    w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = true;
    w.seated     = false;
    EXPECT(Next(w) == Step::Replay);
    w.boxGone = true;
    EXPECT(Next(w) == Step::Lost);
    w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = true;
    w.inCinema   = true;
    w.ranIn      = true;
    EXPECT(Next(w) == Step::Wait);
}

void TestARunThatMissesItsBoxStillFiresIt()
{
    // The AI's path went round the box: at the run's point, or after the limit, the held enter fires (late, never lost).
    Waiting w = kStillWaiting;
    w.awaitsRun  = true;
    w.handedToAi = true;
    EXPECT(Next(w) == Step::Wait);
    w.runOver = true;
    EXPECT(Next(w) == Step::Replay);
    EXPECT(RunOver(150.0f, 0.0f, 0, 0));                    // at the run's point
    EXPECT(!RunOver(5000.0f, 0.0f, 8000, 100));             // the Ch. 5 dash under way
    EXPECT(RunOver(5000.0f, 0.0f, 3000, kRunStillMs));      // stopped short (blocked) for 2 s
    EXPECT(!RunOver(5000.0f, 0.0f, 3000, kRunStillMs - 1));
    EXPECT(RunOver(5000.0f, 0.0f, kAwaitRunLimitMs, 0));    // the cap
}

// TNNL4's TifaRun box and the fence point Tifa's dash runs to (research/.ida-mcp/tifa-barret/data_tnnl4_rows.txt, cm).
constexpr Box kTifaRun{.rotation = {0.0f, 0.0f, 0.0f, 1.0f}, .at = {854.0f, -37606.0f, -4890.0f}, .scale = {1.0f, 1.0f, 1.0f},
                       .extent = {400.0f, 100.0f, 40.0f}};
constexpr float kFencePoint[3] = {490.0f, -38330.0f, -4920.0f};

void TestTheDashFromCloudRunsThroughTheFenceBox()
{
    // Played 2026-10-30 15:57 (run 3): Tifa beside Cloud at box 0195; her AI's dash went through the box.
    const float byCloud[3] = {3221.0f, -32878.0f, -4885.0f};
    EXPECT(RunCrosses(kTifaRun, byCloud, kFencePoint));
    // Run 2: between box 0200 and the fence box.
    const float between[3] = {1200.0f, -36800.0f, -4880.0f};
    EXPECT(RunCrosses(kTifaRun, between, kFencePoint));
}

void TestARunFromPastTheFenceMissesTheBox()
{
    // Run 1: far down the tunnel, past the fence point; the run back never reaches the box.
    const float pastIt[3] = {300.0f, -42000.0f, -4920.0f};
    EXPECT(!RunCrosses(kTifaRun, pastIt, kFencePoint));
    // Beside the box, running parallel to it more than the margin away: misses.
    const float aside[3]  = {854.0f + 400.0f + 60.0f, -37000.0f, -4890.0f};
    const float aside2[3] = {854.0f + 400.0f + 60.0f, -38000.0f, -4890.0f};
    EXPECT(!RunCrosses(kTifaRun, aside, aside2));
    // Within the capsule margin: crosses.
    const float grazing[3]  = {854.0f + 400.0f + 40.0f, -37000.0f, -4890.0f};
    const float grazing2[3] = {854.0f + 400.0f + 40.0f, -38000.0f, -4890.0f};
    EXPECT(RunCrosses(kTifaRun, grazing, grazing2));
}

void TestARunStartingOrEndingInsideCrosses()
{
    const float inside[3] = {854.0f, -37606.0f, -4890.0f};
    EXPECT(RunCrosses(kTifaRun, inside, kFencePoint));
    EXPECT(RunCrosses(kTifaRun, inside, inside));  // standing in it
    const float nan[3] = {std::nanf(""), -37606.0f, -4890.0f};
    EXPECT(!RunCrosses(kTifaRun, nan, kFencePoint));
    const float inf[3] = {std::numeric_limits<float>::infinity(), -37606.0f, -4890.0f};
    EXPECT(!RunCrosses(kTifaRun, inf, kFencePoint));
}

}  // namespace
int main()
{
    TestAMemberOnlyBoxWaitsForAHeldMember();
    TestEverythingElseIsTheGames();
    TestAWaitingEntryWaits();
    TestEachReleaseReplays();
    TestNothingFiresInACutscene();
    TestAGoneBoxIsLostNotReplayed();
    TestInsideAndMarginReach();
    TestTheScaleCounts();
    TestTheRotationCounts();
    TestANanNeverReaches();
    TestABroughtBackMemberWaitsForItsOwnRun();
    TestABroughtBackMemberIsNeverLost();
    TestARunThatMissesItsBoxStillFiresIt();
    TestTheDashFromCloudRunsThroughTheFenceBox();
    TestARunFromPastTheFenceMissesTheBox();
    TestARunStartingOrEndingInsideCrosses();
    return lss::test::Report();
}
