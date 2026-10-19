#include "players/party_seat.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/player_trigger.h"
#include "players/scopes.h"

#include <array>
#include <cstdint>

namespace lss::players::party_seat {

namespace {

using platform::ReadPointer;

using InitPartyLevelFn = void (*)(void* controller, void* trigger);
using SwitchLeaderFn   = void (*)(void* controller, void* character, bool inputTriggered, bool force);

constexpr std::uint32_t kTrigger        = 0x6E0;  // AEndPlayerController: the PlayerTrigger actor
constexpr std::uint32_t kTriggerOverlap = 0x6E8;  // its EndCapsuleOverlapComponent
constexpr std::uint32_t kLeader         = 0x6F8;  // LeaderCharacter

constexpr std::array<std::uint8_t, 16> kInitPartyLevelEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::array<std::uint8_t, 16> kSwitchLeaderEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48};
constexpr platform::FunctionFact kInitPartyLevel{L"EndPlayerController::InitPartyLevel", 0x1415F6E00, kInitPartyLevelEntry};
constexpr platform::FunctionFact kSwitchLeader{L"EndPlayerController::SwitchLeaderInternal", 0x1415F7DB0, kSwitchLeaderEntry};

// Set once at install.
InitPartyLevelFn g_initPartyLevel = nullptr;
SwitchLeaderFn   g_switchLeader   = nullptr;

// Game thread, per seat.
struct State
{
    void*        setUpController = nullptr;  // the controller InitPartyLevel ran on
    void*        member          = nullptr;
    std::uint8_t type            = facts::kPlayerTypeNone;
};
std::array<State, scopes::kMaxPlayers> g_seats;

bool ValidSeat(int seat) noexcept
{
    return seat >= 1 && seat < scopes::kMaxPlayers;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto init   = platform::FindFunction(image, kInitPartyLevel);
    const auto change = platform::FindFunction(image, kSwitchLeader);
    if (!init || !change)
    {
        return false;
    }
    if (!player_trigger::Install(image))
    {
        platform::Log(L"seat: a further player's own trigger cannot be made (an engine check failed); player 1's is borrowed");
    }
    g_initPartyLevel = reinterpret_cast<InitPartyLevelFn>(*init);
    g_switchLeader   = reinterpret_cast<SwitchLeaderFn>(*change);
    return true;
}

bool Seat(int seat, void* playerOneController, void* controller, void* member, std::uint8_t type) noexcept
{
    if (!ValidSeat(seat) || g_seats[seat].member != nullptr || playerOneController == nullptr || controller == nullptr ||
        member == nullptr)
    {
        return false;
    }
    State& state   = g_seats[seat];
    void*  trigger = ReadPointer(playerOneController, kTrigger);
    // The seat's own trigger (a copy of the placed one); player 1's only if none could be made.
    if (void* own = player_trigger::Ensure(playerOneController); own != nullptr)
    {
        trigger = own;
    }
    else
    {
        platform::Log(L"seat {}: borrows player 1's PlayerTrigger (encounters and prompts will follow this seat's member)", seat + 1);
    }
    if (trigger == nullptr || ReadPointer(playerOneController, kTriggerOverlap) == nullptr ||
        member == ReadPointer(playerOneController, kLeader))
    {
        platform::Log(L"seat {}: refused (player 1's trigger or overlap component is missing, or the member is player 1's leader)", seat + 1);
        return false;
    }
    // The game's party setup for a controller, once per controller: it stores the trigger and its overlap
    // component and creates the controller's nav-area component.
    if (state.setUpController != controller)
    {
        if (!platform::CallGuarded(g_initPartyLevel, controller, trigger) || ReadPointer(controller, kTriggerOverlap) == nullptr)
        {
            platform::Log(L"seat {}: InitPartyLevel on its controller failed or left no overlap component; not seating", seat + 1);
            return false;
        }
        state.setUpController = controller;
    }
    if (!platform::CallGuarded(g_switchLeader, controller, member, false, false))
    {
        platform::Log(L"seat {}: SwitchLeaderInternal(controller, member) RAISED AN EXCEPTION", seat + 1);
        return false;
    }
    const bool seated = ReadPointer(controller, kLeader) == member;
    if (seated)
    {
        state.member = member;
        state.type   = type;
        scopes::BindMember(seat, member, type);
    }
    platform::Log(L"seat {}: its controller {} member type {} (its leader field {})", seat + 1,
                  seated ? L"now holds" : L"did NOT take", type, seated ? L"names it" : L"does not name it");
    return seated;
}

bool Unseat(int seat, void* playerOneController, void* controller) noexcept
{
    if (!ValidSeat(seat) || g_seats[seat].member == nullptr || controller == nullptr)
    {
        return true;
    }
    State&     state    = g_seats[seat];
    const bool called   = platform::CallGuarded(g_switchLeader, controller, static_cast<void*>(nullptr), false, false);
    const bool released = ReadPointer(controller, kLeader) == nullptr;
    if (!released)
    {
        platform::Log(L"seat {}: its controller still holds its member after the release{}", seat + 1, called ? L"" : L" (the call raised)");
        return false;
    }
    const std::uint8_t type = state.type;
    state.member            = nullptr;
    state.type              = facts::kPlayerTypeNone;
    scopes::BindMember(seat, nullptr, facts::kPlayerTypeNone);
    // With a borrowed trigger player 1's trigger rode on the member: re-seating player 1's own leader through the
    // same funnel puts it back. The seat's own trigger needs nothing: player 1's never moved.
    const bool borrowed  = !player_trigger::Owns(ReadPointer(controller, kTrigger));
    void*      leaderOne = ReadPointer(playerOneController, kLeader);
    const bool reseated  = borrowed && leaderOne != nullptr &&
                           platform::CallGuarded(g_switchLeader, playerOneController, leaderOne, false, true);
    platform::Log(L"seat {}: member type {} given back to its AI{}; player 1's leader {}", seat + 1, type,
                  called ? L"" : L" (the call raised after releasing it)",
                  borrowed ? (reseated ? L"re-seated (its trigger back on it)" : L"NOT re-seated") : L"untouched (the seat has its own trigger)");
    return true;
}

void Forget(int seat) noexcept
{
    if (!ValidSeat(seat))
    {
        return;
    }
    g_seats[seat] = State{};
    scopes::BindMember(seat, nullptr, facts::kPlayerTypeNone);
    player_trigger::Destroy();
}

bool Seated(int seat) noexcept
{
    return ValidSeat(seat) && g_seats[seat].member != nullptr;
}

void* Member(int seat) noexcept
{
    return ValidSeat(seat) ? g_seats[seat].member : nullptr;
}

std::uint8_t MemberType(int seat) noexcept
{
    return ValidSeat(seat) ? g_seats[seat].type : facts::kPlayerTypeNone;
}

}  // namespace lss::players::party_seat
