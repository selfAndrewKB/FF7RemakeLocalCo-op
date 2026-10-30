// Host-side tests for story_box_rules (world/story_box_rules.h): which story-box entries of a held member wait, when a
// waiting one fires, and the reach test against a placed box. Run without the game. The box is TNNL4's 0200 (Ch. 5
// tunnel, research/.ida-mcp/tifa-barret/data_tnnl4_rows.txt: centre (18.1, -360.3) m, half-size 10 x 2 m).

#include "world/story_box_rules.h"

#include <cmath>
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

constexpr Waiting kStillWaiting{.boxGone = false, .seated = true, .handedToAi = false, .inCinema = false, .keptMove = false,
                                .playerOneReached = false};

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
    w.keptMove = true;
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
    return lss::test::Report();
}
