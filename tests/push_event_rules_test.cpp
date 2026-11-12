// Host-side tests for the push-button rows a player beyond player 1 may work (world/push_event_rules.h). Run without the
// game; the rows are the game's (research/RE-further-player-levers.push-buttons.csv).

#include "world/push_event_rules.h"

#include <cstdio>
#include <string_view>

namespace {

using namespace lss::world::push_event_rules;

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

constexpr std::wstring_view kCloud  = L"Cloud";
constexpr std::wstring_view kTifa   = L"Tifa";
constexpr std::wstring_view kBarret = L"Barret";

/// A shutter lever authored for Cloud (the chapter 3 slums): Cloud works it as written; Tifa and Barret stand at it with
/// no pull animation and the lever moves.
void AOneCharacterLever()
{
    const std::wstring_view actors[]  = {kCloud};
    const std::wstring_view actions[] = {L"CMN_PC0000_00_LeverD01_0", L"CMN_PC0000_00_LeverD01_1", L"CMN_PC0000_00_LeverD01_2", L""};
    const Plan cloud = PlanFor(actors, 1, actions, 4, kCloud);
    EXPECT(cloud.admitted && cloud.asWritten && !cloud.noAnimation);
    const Plan tifa = PlanFor(actors, 1, actions, 4, kTifa);
    EXPECT(tifa.admitted && !tifa.asWritten && tifa.noAnimation);
    const Plan barret = PlanFor(actors, 1, actions, 4, kBarret);
    EXPECT(barret.admitted && barret.noAnimation);
}

/// Tifa's own lever rows (the Shinra building's `Tifa_Area1..4_Lever`) run as written for her.
void ACharactersOwnRow()
{
    const std::wstring_view actors[]  = {kTifa};
    const std::wstring_view actions[] = {L"CMN_PC0002_00_LeverD01_0", L"CMN_PC0002_00_LeverD01_1", L"CMN_PC0002_00_LeverD01_2"};
    const Plan tifa = PlanFor(actors, 1, actions, 3, kTifa);
    EXPECT(tifa.admitted && tifa.asWritten);
    const Plan cloud = PlanFor(actors, 1, actions, 3, kCloud);
    EXPECT(cloud.admitted && cloud.noAnimation);
    EXPECT(PlanFor(actors, 1, actions, 3, L"TIFA").asWritten);  // the names compare without case, as the game's do
}

/// Two characters at once (a lift, a wall), a push of the character's own (a container), a loop animation: player 1's.
void TheStorysRows()
{
    const std::wstring_view two[]     = {kCloud, L"Aerith"};
    const std::wstring_view lift[]    = {L"CMN_PC0000_00_HeavyObjectWall_0", L"", L"", L"", L"CMN_PC0003_00_HeavyObjectWall_0"};
    EXPECT(!PlanFor(two, 2, lift, 5, kCloud).admitted);
    const std::wstring_view one[]     = {kCloud};
    const std::wstring_view push[]    = {L"CMN_PC0000_00_PushContainer_0", L"CMN_PC0000_00_PushContainer_1"};
    EXPECT(!PlanFor(one, 1, push, 2, kCloud).admitted);
    EXPECT(!PlanFor(one, 1, push, 2, kTifa).admitted);
    const std::wstring_view loop[]    = {L"EV_SBIL0_4495_Loop"};
    EXPECT(!PlanFor(one, 1, loop, 1, kTifa).admitted);
    EXPECT(!PlanFor(nullptr, 0, push, 2, kCloud).admitted);  // no character named: not a row the game ships, refused
}

/// A row with a lever animation in one phase and none in the others is a lever (the empty names play nothing).
void EmptyPhases()
{
    const std::wstring_view one[]     = {kCloud};
    const std::wstring_view actions[] = {L"", L"CMN_PC0000_00_LeverU01_1", L"", L""};
    EXPECT(PlanFor(one, 1, actions, 4, kBarret).admitted);
    EXPECT(PlanFor(one, 1, nullptr, 0, kBarret).admitted);  // no animation at all: still a one-character row
    EXPECT(LeverAnimation(L"CMN_PC0001_00_LeverD02_3") && !LeverAnimation(L"CMN_PC0000_00_PushContainer_0") &&
           !LeverAnimation(L"PC0001_00_ReleaseLever"));
}

/// Operation boards by their target name; the elevator switches are told apart.
void TheBoards()
{
    EXPECT(OperationBoard(L"OperationBoard_070-MAKO5_OpB070_B1Door00Sw_In_PressVolume"));
    EXPECT(!ElevatorBoard(L"OperationBoard_070-MAKO5_OpB070_B1Door00Sw_In_PressVolume"));
    EXPECT(ElevatorBoard(L"OperationBoard_140-SBIL0_oblBoard140_59F_ELVSwitch_01_PressVolume"));
    EXPECT(!OperationBoard(L"oblButton030_ShutterLever_00") && !ElevatorBoard(L"Door_01"));
}

}  // namespace

int main()
{
    AOneCharacterLever();
    ACharactersOwnRow();
    TheStorysRows();
    EmptyPhases();
    TheBoards();
    if (g_failures != 0)
    {
        std::printf("push_event_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("push_event_rules: all passed\n");
    return 0;
}
