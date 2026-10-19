#include "battle/party_switch.h"

#include "battle/command_menu_tick.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"

#include <cstdint>

namespace lss::battle::party_switch {

namespace {

using GetPlayerActorFn = void* (*)(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate);

constexpr int          kPlayer   = 1;
constexpr std::uint8_t kTypeNone = facts::kPlayerTypeNone;

std::uintptr_t         g_target        = 0;
std::uintptr_t         g_canSwitchCall = 0;
platform::CallRedirect g_cycleCall;
platform::CallRedirect g_canSwitchCall_;
bool                   g_placed = false;

// Game thread.
std::uint8_t g_lastRefused = kTypeNone;
int          g_lastPlayer  = 0;

/// The member type the switch running now must pass over: inside player 2's tick, player 1's leader; outside it,
/// the member player 2 plays. NONE when there is nothing to exclude.
std::uint8_t Excluded() noexcept
{
    const int ticking = command_menu_tick::TickingPlayer();
    return ticking == 2 ? players::scopes::OtherLeaderType(kPlayer) : players::scopes::MemberType(kPlayer);
}

void* Candidate(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate)
{
    const std::uint8_t excluded = Excluded();
    if (excluded != kTypeNone && type == excluded)
    {
        const int player = command_menu_tick::TickingPlayer() == 2 ? 2 : 1;
        if (g_lastRefused != type || g_lastPlayer != player)
        {
            g_lastRefused = type;
            g_lastPlayer  = player;
            platform::LogDebug(L"party switch: player {}'s switch passes over member type {} (the other player's)", player, type);
        }
        return nullptr;
    }
    return reinterpret_cast<GetPlayerActorFn>(g_target)(partyManager, variant, type, alternate);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto target    = platform::FindFunction(image, facts::kGetPlayerActor);
    const auto cycle     = image.Rebase(facts::kCycleLeaderCandidateCall, 5);
    const auto canSwitch = image.Rebase(facts::kCanSwitchCandidateCall, 5);
    if (!target || !cycle || !canSwitch)
    {
        platform::Log(L"party switch: an engine check failed; a player may switch onto the other's member");
        return false;
    }
    g_target        = *target;
    g_canSwitchCall = *canSwitch;
    if (!g_cycleCall.Install(*cycle, g_target, reinterpret_cast<std::uintptr_t>(&Candidate)))
    {
        platform::Log(L"party switch: the cycle's candidate call could not be redirected; a player may switch onto the other's member");
        return false;
    }
    return true;
}

void Remove() noexcept
{
    g_canSwitchCall_.Restore();
    g_cycleCall.Restore();
}

void Tick() noexcept
{
    if (g_placed || g_canSwitchCall == 0)
    {
        return;
    }
    g_placed = true;
    if (g_canSwitchCall_.InstallFromOwnThread(g_canSwitchCall, g_target, reinterpret_cast<std::uintptr_t>(&Candidate)))
    {
        platform::Log(L"party switch: installed; neither player's switch lands on the other's member");
    }
    else
    {
        platform::Log(L"party switch: CanSwitch's candidate call could not be redirected; a manual switch may count the other's member");
    }
}

}  // namespace lss::battle::party_switch
