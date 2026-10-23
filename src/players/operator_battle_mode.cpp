#include "players/operator_battle_mode.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/player_two_camera.h"
#include "players/seat_objects.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::players::operator_battle_mode {

namespace {

using platform::Read;

// The game calls the enter function with nothing of its own in the argument registers; what it finds there is
// passed on to the flavor restarts, so the replacement passes the same four through.
using EnterFn         = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using PopRequestFn    = void (*)(void* key, void* setting);
using OperatorVoidFn  = void (*)(void* cameraOperator);

// Set at install, then only read.
EnterFn        g_enter           = nullptr;
PopRequestFn   g_popRequest      = nullptr;
OperatorVoidFn g_setStateFree    = nullptr;
OperatorVoidFn g_resetMoveFollow = nullptr;
std::uint8_t** g_data            = nullptr;  // the data singleton (the options block)
std::array<platform::CallRedirect, facts::kFieldCameraEnterBattleModeCalls.size()> g_enterCalls;
platform::CallRedirect g_popCall;

// Game thread.
std::uint32_t g_enters = 0;  // the game's enter calls
std::uint32_t g_leaves = 0;  // the game's battle-end pops
struct Counts
{
    std::uint32_t enters = 0;  // of the game's, run again on the seat's operator
    std::uint32_t leaves = 0;  // of the game's, mirrored on the seat's operator
    std::uint32_t joins  = 0;  // the seat's operator entered battle mode at its spawn
};
std::array<Counts, scopes::kMaxPlayers> g_counts;  // index = seat ([0] unused)

std::uint8_t* Bytes(void* object) noexcept
{
    return static_cast<std::uint8_t*>(object);
}

std::int32_t Mode(void* cameraOperator) noexcept
{
    std::int32_t mode = -1;
    return cameraOperator != nullptr && Read(Bytes(cameraOperator) + facts::kOperatorBattleMode, mode) ? mode : -1;
}

void* Flavor(void* cameraOperator, std::uint32_t type) noexcept
{
    void* flavor = nullptr;
    return Read(Bytes(cameraOperator) + facts::kOperatorFlavors + 8 * type, flavor) ? flavor : nullptr;
}

bool FlavorActive(void* cameraOperator, std::uint32_t type) noexcept
{
    void*        flavor = Flavor(cameraOperator, type);
    std::uint8_t active = 0;
    return flavor != nullptr && Read(Bytes(flavor) + facts::kFlavorActive, active) && active != 0;
}

void SetFlavorActive(void* cameraOperator, std::uint32_t type, std::uint8_t active) noexcept
{
    if (void* flavor = Flavor(cameraOperator, type); flavor != nullptr)
    {
        platform::WriteGuarded(Bytes(flavor) + facts::kFlavorActive, &active, sizeof(active));
    }
}

/// The game's enter function on the seat's operator: inside its operator scope the root weak answers that
/// operator, and the function reads nothing else. False if the seat has no operator bound.
bool EnterSeat(int seat, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4) noexcept
{
    const scopes::Scope scope(seat, scopes::Operator);
    if (!scope.Held())
    {
        return false;
    }
    std::uint64_t result = 0;
    return platform::CallGuarded(g_enter, result, a1, a2, a3, a4);
}

/// The on/off table's battle-off block, step for step, on a seat's operator (RE-operator-settings-two.md §2.3):
/// mode 0, the Battle request popped, Free state, TargetLength and TargetOffset off, SpaceOffset back to following
/// the row when the option says so, the MoveFollow elapsed reset, the field scales and pitch limits selected.
void LeaveSeat(void* seatOperator) noexcept
{
    std::uint8_t*      op    = Bytes(seatOperator);
    const std::int32_t field = 0;
    platform::WriteGuarded(op + facts::kOperatorBattleMode, &field, sizeof(field));
    void*         battle = nullptr;
    std::uint64_t key    = 0;
    if (Read(op + facts::kOperatorSettingBattle, battle) && battle != nullptr)
    {
        platform::CallGuarded(g_popRequest, &key, battle);
    }
    platform::CallGuarded(g_setStateFree, seatOperator);
    SetFlavorActive(seatOperator, facts::kFlavorTargetLength, 0);
    SetFlavorActive(seatOperator, facts::kFlavorTargetOffset, 0);
    std::uint8_t* data   = nullptr;
    std::uint8_t  option = 0;
    if (Read(g_data, data) && data != nullptr && Read(data + facts::kOptionSpaceOffsetInField, option) && option == 1)
    {
        platform::WriteGuarded(op + facts::kOperatorSpaceOffsetByRow, &option, sizeof(option));
    }
    if (FlavorActive(seatOperator, facts::kFlavorMoveFollow))
    {
        platform::CallGuarded(g_resetMoveFollow, seatOperator);
    }
    const std::uintptr_t fieldOption = reinterpret_cast<std::uintptr_t>(op + facts::kOperatorFieldDistanceOption);
    platform::WriteGuarded(op + facts::kOperatorDistanceOptionPtr, &fieldOption, sizeof(fieldOption));
    for (std::uint32_t i = 0; i < 3; ++i)
    {
        std::uintptr_t scale = 0;
        if (Read(op + facts::kOperatorFieldScales + 8 * i, scale))
        {
            platform::WriteGuarded(op + facts::kOperatorActiveScales + 8 * i, &scale, sizeof(scale));
        }
    }
    for (std::uint32_t i = 0; i < 2; ++i)
    {
        std::uintptr_t limit = 0;
        if (Read(op + facts::kOperatorFieldPitchLimits + 8 * i, limit))
        {
            platform::WriteGuarded(op + facts::kOperatorActivePitchLimits + 8 * i, &limit, sizeof(limit));
        }
    }
}

/// At the enter function's call sites: the game's call (the root operator), then the same on each seat's.
std::uint64_t Enter(std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4)
{
    const std::uint64_t result = g_enter(a1, a2, a3, a4);
    ++g_enters;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (EnterSeat(seat, a1, a2, a3, a4))
        {
            ++g_counts[seat].enters;
            platform::LogDebug(L"operator battle mode: {}'s operator entered battle mode with player 1's (modes {} / {})", SeatName(seat),
                               Mode(player_two_camera::OperatorOne()), Mode(seat_objects::Operator(seat)));
        }
    }
    return result;
}

/// At the battle-off block's pop of the Battle request: the game's pop (player 1's operator), then the whole block
/// on each seat's operator that is in battle mode.
void PopRequest(void* key, void* setting)
{
    g_popRequest(key, setting);
    ++g_leaves;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        void* seatOperator = seat_objects::Operator(seat);
        if (seatOperator != nullptr && Mode(seatOperator) == 1)
        {
            LeaveSeat(seatOperator);
            ++g_counts[seat].leaves;
            platform::LogDebug(L"operator battle mode: {}'s operator left battle mode with player 1's (modes {} / {})", SeatName(seat),
                               Mode(player_two_camera::OperatorOne()), Mode(seatOperator));
        }
    }
}

std::wstring Describe(void* cameraOperator) noexcept
{
    if (cameraOperator == nullptr)
    {
        return L"none";
    }
    // A setting's request count: -1 when the setting cannot be read.
    const auto requests = [cameraOperator](std::uint32_t setting) {
        void*        object = nullptr;
        std::int32_t count  = -1;
        if (Read(Bytes(cameraOperator) + setting, object) && object != nullptr)
        {
            Read(Bytes(object) + facts::kOperatorSettingRequests, count);
        }
        return count;
    };
    return std::format(L"mode {} (Battle requests {}, InDoor {}, Town {}, SpaceOffset {}, TargetLength {})", Mode(cameraOperator),
                       requests(facts::kOperatorSettingBattle), requests(facts::kOperatorSettingInDoor), requests(facts::kOperatorSettingTown),
                       FlavorActive(cameraOperator, facts::kFlavorSpaceOffset) ? L"on" : L"off",
                       FlavorActive(cameraOperator, facts::kFlavorTargetLength) ? L"on" : L"off");
}

std::wstring Reading()
{
    std::wstring text = std::format(L"player 1's {}", Describe(player_two_camera::OperatorOne()));
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        text += std::format(L"; {}'s {}", SeatName(seat), Describe(seat_objects::Operator(seat)));
    }
    text += std::format(L"; enters {}, leaves {}", g_enters, g_leaves);
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        text += std::format(L" ({}'s: enters {}, leaves {}, joins in battle {})", SeatName(seat), g_counts[seat].enters, g_counts[seat].leaves,
                            g_counts[seat].joins);
    }
    return text;
}

void RestoreAll() noexcept
{
    g_popCall.Restore();
    for (auto& call : g_enterCalls)
    {
        call.Restore();
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto enter      = platform::FindFunction(image, facts::kFieldCameraEnterBattleMode);
    const auto pop        = platform::FindFunction(image, facts::kCameraSettingPopRequest);
    const auto setFree    = platform::FindFunction(image, facts::kOperatorSetStateFree);
    const auto resetMove  = platform::FindFunction(image, facts::kOperatorResetBattleMoveFollow);
    const auto data       = image.Rebase(facts::kDataSingletonGlobal, sizeof(void*));
    const auto popSite    = image.Rebase(facts::kCameraSettingPopRequestBattleEndCall, 5);
    if (!enter || !pop || !setFree || !resetMove || !data || !popSite)
    {
        platform::Log(L"operator battle mode: an engine check failed; a seat's camera plays battles on the field setting");
        return false;
    }
    g_enter           = reinterpret_cast<EnterFn>(*enter);
    g_popRequest      = reinterpret_cast<PopRequestFn>(*pop);
    g_setStateFree    = reinterpret_cast<OperatorVoidFn>(*setFree);
    g_resetMoveFollow = reinterpret_cast<OperatorVoidFn>(*resetMove);
    g_data            = reinterpret_cast<std::uint8_t**>(*data);

    bool installed = g_popCall.Install(*popSite, *pop, reinterpret_cast<std::uintptr_t>(&PopRequest));
    for (std::size_t i = 0; installed && i < g_enterCalls.size(); ++i)
    {
        const auto site = image.Rebase(facts::kFieldCameraEnterBattleModeCalls[i], 5);
        installed = site && g_enterCalls[i].Install(*site, *enter, reinterpret_cast<std::uintptr_t>(&Enter));
    }
    if (!installed)
    {
        RestoreAll();
        platform::Log(L"operator battle mode: a call site could not be redirected; a seat's camera plays battles on the field setting");
        return false;
    }
    control::Register({.name = L"operator-battle-mode", .shape = control::Shape::Text,
                       .help = L"each operator's battle mode, and how often each seat's entered and left it with player 1's",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"operator battle mode: each seat's operator enters and leaves battle mode with player 1's");
    return true;
}

void Remove() noexcept
{
    RestoreAll();
}

void OnOperatorStarted(int seat, void* seatOperator, void* operatorOne) noexcept
{
    if (g_enter == nullptr || !SeatBuilt(seat) || seatOperator == nullptr || Mode(operatorOne) != 1)
    {
        return;
    }
    if (EnterSeat(seat, 0, 0, 0, 0))
    {
        ++g_counts[seat].joins;
        platform::Log(L"operator battle mode: {}'s operator entered battle mode at its spawn (player 1's is in battle); now {}",
                      SeatName(seat), Describe(seatOperator));
    }
}

}  // namespace lss::players::operator_battle_mode
