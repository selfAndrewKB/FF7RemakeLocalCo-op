#include "battle/party_switch.h"

#include "battle/command_menu_tick.h"
#include "battle/member_state.h"
#include "battle/party_switch_rules.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <atomic>
#include <cstdint>

namespace lss::battle::party_switch {

namespace {

using GetPlayerActorFn = void* (*)(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate);
using CycleLeaderFn    = std::uint64_t (*)(void* system, std::uint64_t direction, std::uint64_t forced, std::uint64_t input);

constexpr std::uint8_t kTypeNone = facts::kPlayerTypeNone;

std::uintptr_t         g_target        = 0;
std::uintptr_t         g_canSwitchSite = 0;
std::uintptr_t         g_koSite        = 0;  // the KO tick's forced call (crosses its block: placed from Tick)
std::uintptr_t         g_cycleLeader   = 0;
platform::CallRedirect g_cycleCall;
platform::CallRedirect g_canSwitchCall;
platform::CallRedirect g_koCall;
std::uintptr_t         g_dpadSites[2]{};  // the d-pad's two calls of the cycle (next, previous): cross their blocks, placed from Tick
platform::CallRedirect g_dpadCalls[2];
bool                   g_placed = false;

// A forced switch refused since the last Tick (written by the hook on the dead-state tick's thread, the game thread
// [ASSUMPTION: the component tick; atomic in case it is a worker]), and on the game thread whether the downed
// leader's episode is logged as running and for how many quiet ticks it has not been refused (its end is logged
// after kQuietTicks, so a pause or a skipped tick does not end it).
constexpr int     kQuietTicks = 30;
std::atomic<bool> g_koRefusedNow{false};
bool              g_koRefusing = false;
int               g_koQuiet    = 0;

// Game thread.
std::uint8_t g_lastRefused = kTypeNone;
int          g_lastSeat    = -1;

/// Whether the switch running now must pass over member `type`, another human's: inside a seat's tick, player 1's
/// leader and every other seat's member; outside it (player 1's switch, the game's KO auto-switch), every seat's
/// member.
bool Excluded(std::uint8_t type, int ticking) noexcept
{
    if (type == kTypeNone)
    {
        return false;
    }
    if (ticking >= 1 && type == players::scopes::OtherLeaderType(ticking))
    {
        return true;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (seat != ticking && players::scopes::MemberType(seat) == type)
        {
            return true;
        }
    }
    return false;
}

void* Candidate(void* partyManager, std::uint8_t variant, std::uint8_t type, bool alternate)
{
    const int seat = command_menu_tick::TickingSeat();  // 0: player 1's switch
    if (Excluded(type, seat))
    {
        if (g_lastRefused != type || g_lastSeat != seat)
        {
            g_lastRefused = type;
            g_lastSeat    = seat;
            platform::LogDebug(L"party switch: {}'s switch passes over member type {} (another player's)", players::SeatName(seat), type);
        }
        return nullptr;
    }
    return reinterpret_cast<GetPlayerActorFn>(g_target)(partyManager, variant, type, alternate);
}

/// The KO'd leader's forced switch: refused in co-op, so player 1 stays on the downed leader until a revive (the
/// game's own "nobody to take" result: no leader write, no retry, no re-target). Single player: the game's switch.
std::uint64_t KoAutoSwitch(void* system, std::uint64_t direction, std::uint64_t forced, std::uint64_t input)
{
    if (players::AnyFurtherMember())
    {
        g_koRefusedNow.store(true, std::memory_order_relaxed);
        platform::LogTrace(L"party switch: the downed leader's forced switch refused");
        return 0;
    }
    return reinterpret_cast<CycleLeaderFn>(g_cycleLeader)(system, direction, forced, input);
}

/// The leader whose switch runs now (a seat's member inside its tick, the party manager's leader slot being the seat's
/// there; player 1's character otherwise) lies knocked out. An unreadable state is not down: the game's gate stays.
bool LeaderDown() noexcept
{
    return member_state::KnockedOut(players::field_root::PartyLeader(facts::kLeaderCurrent), false);
}

int g_cyclingSeat = 0;  // the seat whose d-pad cycle runs now (game thread), 0 outside one and for player 1's

/// The d-pad's call of the cycle, player 1's and, inside its tick, each seat's: the game's cycle, run as its forced switch
/// runs it when in co-op the pressing player's character lies knocked out (party_switch_rules::ForcedCycle).
std::uint64_t RequestedCycle(void* system, std::uint64_t direction, std::uint64_t forced, std::uint64_t input)
{
    const int           seat  = command_menu_tick::TickingSeat();  // 0: player 1's press
    const bool          coop  = seat != 0 || players::AnyFurtherMember();
    const bool          down  = coop && LeaderDown();
    const std::uint64_t force = party_switch_rules::ForcedCycle(coop, down) ? 1 : forced;
    g_cyclingSeat             = seat;
    const std::uint64_t taken = reinterpret_cast<CycleLeaderFn>(g_cycleLeader)(system, direction, force, input);
    g_cyclingSeat             = 0;
    if (!coop)
    {
        return taken;
    }
    if (down)
    {
        platform::Log(L"party switch: {}'s character is down; their own switch {} (the game's two-standing gate dropped, as its own "
                      L"forced switch drops it)",
                      players::SeatName(seat), taken != 0 ? L"takes another member" : L"finds nobody to take");
    }
    else
    {
        platform::LogDebug(L"party switch: {} asked to switch to the {} member: {}", players::SeatName(seat),
                           direction != 0 ? L"next" : L"previous", taken != 0 ? L"taken" : L"refused by the game's cycle");
    }
    return taken;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto target    = platform::FindFunction(image, facts::kGetPlayerActor);
    const auto cycle     = image.Rebase(facts::kCycleLeaderCandidateCall, 5);
    const auto canSwitch = image.Rebase(facts::kCanSwitchCandidateCall, 5);
    const auto koSite    = image.Rebase(facts::kKoAutoSwitchCall, 5);
    const auto cycleFn   = platform::FindFunction(image, facts::kCycleLeader);
    const auto dpadNext  = image.Rebase(facts::kDpadSwitchNextCall, 5);
    const auto dpadPrev  = image.Rebase(facts::kDpadSwitchPreviousCall, 5);
    if (koSite && cycleFn)
    {
        g_koSite      = *koSite;
        g_cycleLeader = *cycleFn;
    }
    if (cycleFn && dpadNext && dpadPrev)
    {
        g_dpadSites[0] = *dpadNext;
        g_dpadSites[1] = *dpadPrev;
    }
    if (!target || !cycle || !canSwitch)
    {
        platform::Log(L"party switch: an engine check failed; a player may switch onto the other's member");
        return false;
    }
    g_target        = *target;
    g_canSwitchSite = *canSwitch;
    if (!g_cycleCall.Install(*cycle, g_target, reinterpret_cast<std::uintptr_t>(&Candidate)))
    {
        platform::Log(L"party switch: the cycle's candidate call could not be redirected; a player may switch onto the other's member");
        return false;
    }
    return true;
}

int CyclingSeat() noexcept
{
    return g_cyclingSeat;
}

void Remove() noexcept
{
    g_canSwitchCall.Restore();
    g_cycleCall.Restore();
    // The KO tick's crossing call and the d-pad's two stay (only the thread that runs them could put them back); the DLL
    // is pinned for them (platform/call_redirect.h), and each passes the game's call through with no further player in.
}

void Tick() noexcept
{
    const bool refused = g_koRefusedNow.exchange(false, std::memory_order_relaxed);
    g_koQuiet          = refused ? 0 : g_koQuiet + 1;
    if (refused && !g_koRefusing)
    {
        g_koRefusing = true;
        platform::LogDebug(L"party switch: player 1's leader is down; player 1 stays on it until a revive (co-op: no forced switch)");
    }
    else if (g_koRefusing && g_koQuiet >= kQuietTicks)
    {
        g_koRefusing = false;
        platform::LogDebug(L"party switch: the downed leader's forced switch has not run for {} ticks (revived, the battle ended, or single player)",
                           kQuietTicks);
    }
    if (g_placed || g_canSwitchSite == 0)
    {
        return;
    }
    g_placed = true;
    if (g_canSwitchCall.InstallFromOwnThread(g_canSwitchSite, g_target, reinterpret_cast<std::uintptr_t>(&Candidate)))
    {
        platform::Log(L"party switch: installed; neither player's switch lands on the other's member");
    }
    else
    {
        platform::Log(L"party switch: CanSwitch's candidate call could not be redirected; a manual switch may count the other's member");
    }
    if (g_koSite != 0 && g_koCall.InstallFromOwnThread(g_koSite, g_cycleLeader, reinterpret_cast<std::uintptr_t>(&KoAutoSwitch)))
    {
        platform::Log(L"party switch: in co-op a downed leader's forced switch is refused; every player stays on their character until revived");
    }
    else
    {
        platform::Log(L"party switch: the KO tick's forced switch could not be redirected; a downed player 1 is moved to a standing AI ally");
    }
    // The d-pad's two calls together, or neither: one direction dropping the gate and the other not would puzzle a player.
    bool dpad = g_cycleLeader != 0;
    for (std::size_t i = 0; dpad && i < 2; ++i)
    {
        dpad = g_dpadSites[i] != 0 &&
               g_dpadCalls[i].InstallFromOwnThread(g_dpadSites[i], g_cycleLeader, reinterpret_cast<std::uintptr_t>(&RequestedCycle));
    }
    if (dpad)
    {
        platform::Log(L"party switch: a downed player's own d-pad switch takes a standing AI member (the game's two-standing gate dropped "
                      L"in co-op)");
    }
    else
    {
        g_dpadCalls[0].Restore();  // a crossing call put back from the thread that placed it, between the ticks
        platform::Log(L"party switch: the d-pad's calls could not be redirected; a downed player's switch stays the game's refusal");
    }
}

}  // namespace lss::battle::party_switch
