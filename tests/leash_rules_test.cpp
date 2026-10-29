// Host-side tests for leash_rules (players/leash_rules.h): whether a live AutoWalk act leashes player 1 to a further
// player's member. Run without the game. The rows are the paks' (research/.ida-mcp/lead-unlock/data_slum7_rows.txt,
// data_type56_autowalk_acts_all_fields.txt).

#include "players/leash_rules.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "leash_rules: all passed\n" : "leash_rules: %d failure(s)\n", g_failures);
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

using namespace lss::players::leash_rules;

// act030_SetMode_2010_Lead: ints [1, 1, 1, 0, 1, 0, 1, 1, 1500].
constexpr AutoWalkAct kTifaCh4{L"obj030_PlayerModeManager", L"scg030_PlayerMode_2010_Lead", L"Tifa", L"opy030_Tifa", 1, 1};
// act080_SLU5B_20_AutoFollowWalk_Aerith: ints [0, 0, 0, 0, 1, 0, 1, 1200, 1400].
constexpr AutoWalkAct kAerithCh8{L"obj080_SLU5B_20_FollowWalkManager", L"scg080_SLU5B_20_AutoFollowWalk_Enable", L"Aerith",
                                 L"opy080_Aerith", 1, 1};

void TestTifasLeadIsLeashed()
{
    EXPECT(LeashesMember(kTifaCh4, L"obj030_PlayerModeManager", L"scg030_PlayerMode_2010_Lead", L"Tifa", L"opy030_Tifa"));
}

void TestAerithsWalkHomeIsLeashed()
{
    EXPECT(LeashesMember(kAerithCh8, L"obj080_SLU5B_20_FollowWalkManager", L"scg080_SLU5B_20_AutoFollowWalk_Enable", L"Aerith",
                         L"opy080_Aerith"));
}

void TestTheLookAtObjectAloneIsEnough()
{
    // A member whose tag could not be read is still found by its level object.
    EXPECT(LeashesMember(kTifaCh4, L"obj030_PlayerModeManager", L"scg030_PlayerMode_2010_Lead", L"", L"opy030_Tifa"));
}

void TestTheTagAloneIsEnough()
{
    // act080_SLU5A_50_SetWalkOochiSys_02: Barret, no look-at object, ints [300, 1000, 2000, 0, 1, ...].
    constexpr AutoWalkAct barret{L"obj080_SLU5A_Manager", L"scg080_SLU5A_50_Walk", L"Barret", L"", 1, 0};
    EXPECT(LeashesMember(barret, L"obj080_SLU5A_Manager", L"scg080_SLU5A_50_Walk", L"Barret", L"opy080_Barret"));
}

void TestAnotherMemberIsNotLeashed()
{
    EXPECT(!LeashesMember(kTifaCh4, L"obj030_PlayerModeManager", L"scg030_PlayerMode_2010_Lead", L"Barret", L"opy030_Barret"));
}

void TestAPointTargetWithoutTurnBackIsNot()
{
    // Barret's Ch. 7 walk-ups: the area's AutoWalk acts target points and have no integer arguments.
    constexpr AutoWalkAct mako{L"obj070_PlayerModeManager", L"scg070_PlayerMode_0130", L"oblPoint070_PV_0130", L"", 0, 0};
    EXPECT(!LeashesMember(mako, L"obj070_PlayerModeManager", L"scg070_PlayerMode_0130", L"Barret", L"opy070_Barret"));
}

void TestAWalkOnTheMemberWithoutTurnBackOrLookIsNot()
{
    // act020_SetPlayerMode_AutoWalk1: target Barret, only the band edges [600, 1500, 3000].
    constexpr AutoWalkAct speedOnly{L"obj020_PlayerModeManager", L"scg020_PlayerMode_AutoWalk1", L"Barret", L"", 0, 0};
    EXPECT(!LeashesMember(speedOnly, L"obj020_PlayerModeManager", L"scg020_PlayerMode_AutoWalk1", L"Barret", L"opy020_Barret"));
}

void TestAnActWhoseStateHasMovedOnDoesNotRun()
{
    EXPECT(!LeashesMember(kTifaCh4, L"obj030_PlayerModeManager", L"scg030_PlayerMode_2010_End", L"Tifa", L"opy030_Tifa"));
    EXPECT(!LeashesMember(kTifaCh4, L"obj030_OtherManager", L"scg030_PlayerMode_2010_Lead", L"Tifa", L"opy030_Tifa"));
}

void TestNamesCompareWithoutCase()
{
    EXPECT(LeashesMember(kTifaCh4, L"OBJ030_PlayerModeManager", L"scg030_playermode_2010_lead", L"TIFA", L""));
    EXPECT(SameName(L"Tifa", L"tifa"));
    EXPECT(!SameName(L"", L""));
    EXPECT(!SameName(L"Tifa", L"Tifa2"));
}

}  // namespace

int main()
{
    TestTifasLeadIsLeashed();
    TestAerithsWalkHomeIsLeashed();
    TestTheLookAtObjectAloneIsEnough();
    TestTheTagAloneIsEnough();
    TestAnotherMemberIsNotLeashed();
    TestAPointTargetWithoutTurnBackIsNot();
    TestAWalkOnTheMemberWithoutTurnBackOrLookIsNot();
    TestAnActWhoseStateHasMovedOnDoesNotRun();
    TestNamesCompareWithoutCase();
    return lss::test::Report();
}
