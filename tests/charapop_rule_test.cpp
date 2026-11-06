// Host-side tests for charapop::KeepsAwakeOrShown, ShouldRaise and HoldOf, run without the game.
//
// The Middle parameters the cases use: FOV 75, draw 10000, in 6000, out 3000, should-tick 1000.

#include "world/charapop_rule.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "charapop_rule: all passed\n" : "charapop_rule: %d failure(s)\n", g_failures);
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

using lss::world::charapop::EntryView;
using lss::world::charapop::Hold;
using lss::world::charapop::HoldOf;
using lss::world::charapop::KeepsAwakeOrShown;
using lss::world::charapop::LodDistances;
using lss::world::charapop::ShouldRaise;

constexpr LodDistances kMiddle{75.0f, 1000.0f, 6000.0f, 3000.0f, 10000.0f, 2000.0f};

void TestShouldTick()
{
    std::puts("rule: within the should-tick distance, awake in any direction");
    EXPECT(KeepsAwakeOrShown(kMiddle, {900.0f, 180.0f}));
    EXPECT(KeepsAwakeOrShown(kMiddle, {1000.0f, 180.0f}));
}

void TestCone()
{
    std::puts("rule: the in-camera distance inside the cone, the out-of-camera one outside");
    EXPECT(KeepsAwakeOrShown(kMiddle, {5000.0f, 30.0f}));
    EXPECT(!KeepsAwakeOrShown(kMiddle, {6500.0f, 30.0f}));
    EXPECT(!KeepsAwakeOrShown(kMiddle, {5000.0f, 120.0f}));
    EXPECT(KeepsAwakeOrShown(kMiddle, {2900.0f, 120.0f}));
    EXPECT(KeepsAwakeOrShown(kMiddle, {5000.0f, 75.0f}));   // the cone's edge is inside
    EXPECT(!KeepsAwakeOrShown(kMiddle, {5000.0f, 76.0f}));
}

void TestEnemyAndHide()
{
    std::puts("rule: the enemy bonus widens the in-camera distance; hiding entries stay shown within the draw distance");
    EXPECT(KeepsAwakeOrShown(kMiddle, {7500.0f, 10.0f, false, true}));
    EXPECT(!KeepsAwakeOrShown(kMiddle, {7500.0f, 10.0f, false, false}));
    EXPECT(KeepsAwakeOrShown(kMiddle, {9000.0f, 150.0f, true, false}));
    EXPECT(!KeepsAwakeOrShown(kMiddle, {11000.0f, 150.0f, true, false}));
}

void TestMargin()
{
    std::puts("rule: the keep margin widens distances and the cone");
    EXPECT(!KeepsAwakeOrShown(kMiddle, {6400.0f, 30.0f}));
    EXPECT(KeepsAwakeOrShown(kMiddle, {6400.0f, 30.0f}, 1.1f));
    EXPECT(KeepsAwakeOrShown(kMiddle, {5000.0f, 78.0f}, 1.0f, 5.0f));
}

void TestRaiseOnlyWhatPlayerOneLacks()
{
    std::puts("raise: only what player 2's view keeps and player 1's camera does not");
    const EntryView near{2000.0f, 10.0f};
    EXPECT(ShouldRaise(kMiddle, near, nullptr, 1.0f));               // player 1's camera unread: as before
    const EntryView oneFar{20000.0f, 10.0f};
    EXPECT(ShouldRaise(kMiddle, near, &oneFar, 1.0f));               // apart
    const EntryView oneNear{2500.0f, 20.0f};
    EXPECT(!ShouldRaise(kMiddle, near, &oneNear, 1.0f));             // together
    const EntryView oneBehind{4000.0f, 150.0f};
    EXPECT(ShouldRaise(kMiddle, near, &oneBehind, 1.0f));            // close to player 1 but behind its camera
    const EntryView twoFar{20000.0f, 10.0f};
    EXPECT(!ShouldRaise(kMiddle, twoFar, &oneFar, 1.0f));            // neither needs it
}

void TestRaiseHidingEntries()
{
    std::puts("raise: a hiding entry player 1 keeps awake but not shown is still raised");
    const EntryView two{8000.0f, 150.0f, true, false};  // asleep for player 2, but shown
    const EntryView one{900.0f, 150.0f, true, false};   // awake and shown for player 1
    EXPECT(!ShouldRaise(kMiddle, two, &one, 1.0f));
    const EntryView twoAwake{900.0f, 10.0f, true, false};
    const EntryView oneShownOnly{9000.0f, 150.0f, true, false};
    EXPECT(ShouldRaise(kMiddle, twoAwake, &oneShownOnly, 1.0f));  // player 2 needs it awake too
}

void TestReducedReach()
{
    std::puts("raise: the reduced reach halves player 2's distances");
    const EntryView mid{4000.0f, 10.0f};
    EXPECT(ShouldRaise(kMiddle, mid, nullptr, 1.0f));
    EXPECT(!ShouldRaise(kMiddle, mid, nullptr, 0.5f));
    EXPECT(ShouldRaise(kMiddle, {2500.0f, 10.0f}, nullptr, 0.5f));
}

void TestHold()
{
    std::puts("hold: released past player 2's reach, or once player 1's camera keeps it with room to spare");
    const EntryView two{6400.0f, 30.0f};
    EXPECT(HoldOf(kMiddle, two, nullptr, 1.0f) == Hold::Keep);           // within the 10% margin
    EXPECT(HoldOf(kMiddle, {6700.0f, 30.0f}, nullptr, 1.0f) == Hold::OutOfReach);
    const EntryView near{2000.0f, 10.0f};
    const EntryView oneEdge{5800.0f, 30.0f};                             // inside player 1's 6000, not 5455
    EXPECT(HoldOf(kMiddle, near, &oneEdge, 1.0f) == Hold::Keep);
    const EntryView oneConeEdge{5000.0f, 73.0f};                         // inside 75 deg, not 70
    EXPECT(HoldOf(kMiddle, near, &oneConeEdge, 1.0f) == Hold::Keep);
    const EntryView oneClose{3000.0f, 20.0f};
    EXPECT(HoldOf(kMiddle, near, &oneClose, 1.0f) == Hold::PlayerOneCovers);
}

void TestSeveralViews()
{
    std::puts("several further players: raised when any view needs it; kept while any still does; covered only when "
              "player 1's camera covers every view that reaches it");
    const EntryView near{2000.0f, 10.0f};
    const EntryView far{20000.0f, 10.0f};
    const EntryView oneFar{20000.0f, 10.0f};
    const EntryView oneClose{3000.0f, 20.0f};
    const EntryView twoNear[] = {far, near};
    const EntryView bothFar[] = {far, far};
    EXPECT(ShouldRaise(kMiddle, twoNear, 2, &oneFar, 1.0f));     // player 3's view needs it, player 2's does not
    EXPECT(ShouldRaise(kMiddle, twoNear, 2, nullptr, 1.0f));
    EXPECT(!ShouldRaise(kMiddle, twoNear, 2, &oneClose, 1.0f));  // player 1's camera covers what player 3 asks
    EXPECT(!ShouldRaise(kMiddle, bothFar, 2, &oneFar, 1.0f));    // neither needs it
    EXPECT(!ShouldRaise(kMiddle, twoNear, 0, &oneFar, 1.0f));    // no further player: nothing to raise
    EXPECT(ShouldRaise(kMiddle, twoNear + 1, 1, &oneFar, 1.0f) == ShouldRaise(kMiddle, near, &oneFar, 1.0f));
    EXPECT(ShouldRaise(kMiddle, twoNear + 1, 1, &oneClose, 1.0f) == ShouldRaise(kMiddle, near, &oneClose, 1.0f));
    // A hiding entry player 1 only shows: player 2's view asks the same (covered), player 3's needs it awake (not).
    const EntryView oneShownOnly{9000.0f, 150.0f, true, false};
    const EntryView shownOnly{8000.0f, 150.0f, true, false};
    const EntryView awakeToo{900.0f, 10.0f, true, false};
    const EntryView coveredAndNot[] = {shownOnly, awakeToo};
    EXPECT(!ShouldRaise(kMiddle, coveredAndNot, 1, &oneShownOnly, 1.0f));  // player 2's view alone: covered
    EXPECT(ShouldRaise(kMiddle, coveredAndNot, 2, &oneShownOnly, 1.0f));   // with player 3's: raised

    const EntryView edge{6400.0f, 30.0f};   // within the keep margin
    const EntryView gone{6700.0f, 30.0f};   // past it
    const EntryView keepAndGone[] = {gone, edge};
    const EntryView bothGone[] = {gone, gone};
    const EntryView nearAndGone[] = {near, gone};
    EXPECT(HoldOf(kMiddle, keepAndGone, 2, nullptr, 1.0f) == Hold::Keep);        // player 3's view still needs it
    EXPECT(HoldOf(kMiddle, bothGone, 2, nullptr, 1.0f) == Hold::OutOfReach);     // nobody's does
    EXPECT(HoldOf(kMiddle, nearAndGone, 2, &oneClose, 1.0f) == Hold::PlayerOneCovers);  // player 1 covers the one that reaches
    EXPECT(HoldOf(kMiddle, nearAndGone, 2, &oneFar, 1.0f) == Hold::Keep);        // and not when it does not
    EXPECT(HoldOf(kMiddle, nearAndGone, 0, &oneFar, 1.0f) == Hold::OutOfReach);  // no further player: released
    EXPECT(HoldOf(kMiddle, nearAndGone, 1, &oneClose, 1.0f) == HoldOf(kMiddle, near, &oneClose, 1.0f));
    EXPECT(HoldOf(kMiddle, bothGone, 1, nullptr, 1.0f) == HoldOf(kMiddle, gone, nullptr, 1.0f));  // OutOfReach either way
    EXPECT(HoldOf(kMiddle, keepAndGone + 1, 1, nullptr, 1.0f) == HoldOf(kMiddle, edge, nullptr, 1.0f));  // Keep either way
    const EntryView coveredThenKept[] = {shownOnly, awakeToo};  // player 1 covers player 2's view; player 3's still needs it
    EXPECT(HoldOf(kMiddle, coveredThenKept, 1, &oneShownOnly, 1.0f) == Hold::PlayerOneCovers);
    EXPECT(HoldOf(kMiddle, coveredThenKept, 2, &oneShownOnly, 1.0f) == Hold::Keep);
}

}  // namespace

int main()
{
    TestShouldTick();
    TestCone();
    TestEnemyAndHide();
    TestMargin();
    TestRaiseOnlyWhatPlayerOneLacks();
    TestRaiseHidingEntries();
    TestReducedReach();
    TestHold();
    TestSeveralViews();
    return lss::test::Report();
}
