#include "players/party_seat.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"

#include <array>
#include <cstdint>

namespace lss::players::party_seat {

namespace {

using InitPartyLevelFn = void (*)(void* controller, void* trigger);
using SwitchLeaderFn   = void (*)(void* controller, void* character, bool inputTriggered, bool force);
using GetPlayerActorFn = void* (*)(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate);

constexpr std::uint32_t kTrigger       = 0x6E0;  // AEndPlayerController: the PlayerTrigger actor
constexpr std::uint32_t kTriggerOverlap = 0x6E8;  // its EndCapsuleOverlapComponent
constexpr std::uint32_t kLeader        = 0x6F8;  // LeaderCharacter

constexpr std::array<std::uint8_t, 16> kInitPartyLevelEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::array<std::uint8_t, 16> kSwitchLeaderEntry{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48};
constexpr platform::FunctionFact kInitPartyLevel{L"EndPlayerController::InitPartyLevel", 0x1415F6E00, kInitPartyLevelEntry};
constexpr platform::FunctionFact kSwitchLeader{L"EndPlayerController::SwitchLeaderInternal", 0x1415F7DB0, kSwitchLeaderEntry};

// Set once at install.
InitPartyLevelFn g_initPartyLevel = nullptr;
SwitchLeaderFn   g_switchLeader   = nullptr;
GetPlayerActorFn g_getPlayerActor = nullptr;
void**           g_partyManager   = nullptr;

// Game thread.
void* g_seatedController = nullptr;  // player 2's controller once InitPartyLevel ran on it
bool  g_seated           = false;

void* ReadPointer(const void* object, std::uint32_t offset) noexcept
{
    void* value = nullptr;
    return object != nullptr && platform::CopyGuarded(&value, static_cast<const std::uint8_t*>(object) + offset,
                                                      sizeof(value))
               ? value
               : nullptr;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto init   = platform::FindFunction(image, kInitPartyLevel);
    const auto change = platform::FindFunction(image, kSwitchLeader);
    const auto find   = platform::FindFunction(image, facts::kGetPlayerActor);
    const auto party  = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    if (!init || !change || !find || !party)
    {
        return false;
    }
    g_initPartyLevel = reinterpret_cast<InitPartyLevelFn>(*init);
    g_switchLeader   = reinterpret_cast<SwitchLeaderFn>(*change);
    g_getPlayerActor = reinterpret_cast<GetPlayerActorFn>(*find);
    g_partyManager   = reinterpret_cast<void**>(*party);
    return true;
}

bool Seat(void* playerOneController, void* playerTwoController) noexcept
{
    if (g_seated || playerOneController == nullptr || playerTwoController == nullptr)
    {
        return false;
    }
    void* trigger = ReadPointer(playerOneController, kTrigger);
    void* manager = ReadPointer(g_partyManager, 0);
    void* aerith  = nullptr;
    if (trigger == nullptr || ReadPointer(playerOneController, kTriggerOverlap) == nullptr || manager == nullptr ||
        !platform::CallGuarded(g_getPlayerActor, aerith, manager, std::uint8_t{0}, facts::kAerithType, false) ||
        aerith == nullptr || aerith == ReadPointer(playerOneController, kLeader))
    {
        platform::Log(L"seat probe: refused (player 1's trigger, the party manager or Aerith is missing, or she is player 1's leader)");
        return false;
    }
    // The game's party setup for a controller, once per controller: it stores the trigger and its overlap
    // component and creates the controller's nav-area component.
    if (g_seatedController != playerTwoController)
    {
        if (!platform::CallGuarded(g_initPartyLevel, playerTwoController, trigger) ||
            ReadPointer(playerTwoController, kTriggerOverlap) == nullptr)
        {
            platform::Log(L"seat probe: InitPartyLevel on player 2's controller failed or left no overlap component; not seating");
            return false;
        }
        g_seatedController = playerTwoController;
    }
    if (!platform::CallGuarded(g_switchLeader, playerTwoController, aerith, false, false))
    {
        platform::Log(L"seat probe: SwitchLeaderInternal(player 2, Aerith) RAISED AN EXCEPTION");
        return false;
    }
    g_seated = ReadPointer(playerTwoController, kLeader) == aerith;
    if (g_seated)
    {
        scopes::BindMember(1, aerith, facts::kAerithType);
    }
    platform::Log(L"seat probe: player 2's controller {} Aerith (its leader field {})", g_seated ? L"now holds" : L"did NOT take",
                  g_seated ? L"names her" : L"does not name her");
    return g_seated;
}

bool Unseat(void* playerOneController, void* playerTwoController) noexcept
{
    if (!g_seated || playerTwoController == nullptr)
    {
        return true;
    }
    const bool called   = platform::CallGuarded(g_switchLeader, playerTwoController, static_cast<void*>(nullptr), false, false);
    const bool released = ReadPointer(playerTwoController, kLeader) == nullptr;
    if (!released)
    {
        platform::Log(L"seat probe: player 2's controller still holds Aerith after the release{}", called ? L"" : L" (the call raised)");
        return false;
    }
    g_seated = false;
    scopes::BindMember(1, nullptr, 9);
    // The shared trigger rode on Aerith: re-seating player 1's own leader through the same funnel puts it back.
    void*      leaderOne = ReadPointer(playerOneController, kLeader);
    const bool reseated  = leaderOne != nullptr &&
                           platform::CallGuarded(g_switchLeader, playerOneController, leaderOne, false, true);
    platform::Log(L"seat probe: Aerith given back to her AI{}; player 1's leader {}", called ? L"" : L" (the call raised after releasing her)",
                  reseated ? L"re-seated (trigger back on it)" : L"NOT re-seated");
    return true;
}

void Forget() noexcept
{
    g_seatedController = nullptr;
    g_seated           = false;
    scopes::BindMember(1, nullptr, 9);
}

bool Seated() noexcept
{
    return g_seated;
}

}  // namespace lss::players::party_seat
