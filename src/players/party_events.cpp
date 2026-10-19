#include "players/party_events.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <array>
#include <cstdint>
#include <format>
#include <span>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::players::party_events {

namespace {

using SetLeaderFn     = std::int64_t (*)(void* partyManager, void* character, std::uint8_t input, std::uint8_t force);
using SetLeaderToadFn = std::int64_t (*)(void* partyManager, void* character);
using StreamFn        = std::int64_t (*)(void* partyManager);
using HandFn          = char (*)();
using ReturnFn        = std::int64_t (*)(void* stateManager);

// Set once at install.
Listener                g_listener;
std::uintptr_t          g_setLeader = 0, g_setLeaderToad = 0;
platform::EntryRedirect g_leader, g_leaderToad;
platform::CallRedirect  g_stream;
std::array<platform::CallRedirect, 2> g_hands;
platform::CallRedirect  g_return;
DWORD                   g_thread = 0;
bool                    g_placed = false;

struct Counts
{
    std::uint32_t leader = 0, stream = 0, handed = 0, returned = 0, offThread = 0;
};
Counts g_counts;

bool OnGameThread() noexcept
{
    if (g_thread != 0 && GetCurrentThreadId() == g_thread)
    {
        return true;
    }
    ++g_counts.offThread;
    return false;
}

std::int64_t SetLeader(void* partyManager, void* character, std::uint8_t input, std::uint8_t force)
{
    if (OnGameThread() && g_listener.leaderChanging != nullptr)
    {
        ++g_counts.leader;
        g_listener.leaderChanging(g_listener.context, character);
    }
    return reinterpret_cast<SetLeaderFn>(g_leader.Original())(partyManager, character, input, force);
}

std::int64_t SetLeaderToad(void* partyManager, void* character)
{
    if (OnGameThread() && g_listener.leaderChanging != nullptr)
    {
        ++g_counts.leader;
        g_listener.leaderChanging(g_listener.context, character);
    }
    return reinterpret_cast<SetLeaderToadFn>(g_leaderToad.Original())(partyManager, character);
}

std::int64_t FinishStream(void* partyManager)
{
    const bool heard = OnGameThread();
    if (heard && g_listener.streamFinishing != nullptr)
    {
        g_listener.streamFinishing(g_listener.context);
    }
    const std::int64_t result = reinterpret_cast<StreamFn>(g_stream.Original())(partyManager);
    if (heard)
    {
        ++g_counts.stream;
        if (g_listener.streamFinished != nullptr)
        {
            g_listener.streamFinished(g_listener.context);
        }
    }
    return result;
}

char HandLeaderToAi()
{
    const char result = reinterpret_cast<HandFn>(g_hands[0].Original())();
    if (OnGameThread() && g_listener.leaderHandedToAi != nullptr)
    {
        ++g_counts.handed;
        g_listener.leaderHandedToAi(g_listener.context);
    }
    return result;
}

std::int64_t ReturnLeader(void* stateManager)
{
    const std::int64_t result = reinterpret_cast<ReturnFn>(g_return.Original())(stateManager);
    if (OnGameThread() && g_listener.leaderReturned != nullptr)
    {
        ++g_counts.returned;
        g_listener.leaderReturned(g_listener.context);
    }
    return result;
}

}  // namespace

bool Install(const platform::GameImage& image, const Listener& listener) noexcept
{
    const auto setLeader = platform::FindFunction(image, facts::kSetPartyLeader);
    const auto setToad   = platform::FindFunction(image, facts::kSetPartyLeaderToad);
    const auto stream    = platform::FindFunction(image, facts::kFinishPlayerStream);
    const auto streamJmp = image.Rebase(facts::kFinishPlayerStreamJump, 5);
    const auto hand      = platform::FindFunction(image, facts::kHandLeaderToAi);
    const auto ret       = platform::FindFunction(image, facts::kReturnLeaderToPc);
    const auto retCall   = image.Rebase(facts::kReturnLeaderCall, 5);
    std::array<std::optional<std::uintptr_t>, 2> handCalls;
    for (std::size_t i = 0; i < handCalls.size(); ++i)
    {
        handCalls[i] = image.Rebase(facts::kHandLeaderCalls[i], 5);
    }
    if (!setLeader || !setToad || !stream || !streamJmp || !hand || !ret || !retCall || !handCalls[0] || !handCalls[1])
    {
        platform::Log(L"party events: an engine check failed; the game's party funnels are not heard");
        return false;
    }
    g_listener      = listener;
    g_setLeader     = *setLeader;
    g_setLeaderToad = *setToad;
    // The call sites together: a funnel heard by halves would give back at one change and not the next.
    const bool installed = g_stream.InstallTailJump(*streamJmp, *stream, reinterpret_cast<std::uintptr_t>(&FinishStream)) &&
                           g_hands[0].Install(*handCalls[0], *hand, reinterpret_cast<std::uintptr_t>(&HandLeaderToAi)) &&
                           g_hands[1].Install(*handCalls[1], *hand, reinterpret_cast<std::uintptr_t>(&HandLeaderToAi)) &&
                           g_return.Install(*retCall, *ret, reinterpret_cast<std::uintptr_t>(&ReturnLeader));
    if (!installed)
    {
        Remove();
        platform::Log(L"party events: a call site could not be redirected; the game's party funnels are not heard");
        return false;
    }
    control::Register({.name = L"party-events", .shape = control::Shape::Text,
                       .help = L"the game's party funnels heard: leader changes, formation rebuilds, the leader handed to its AI "
                               L"and returned, calls off the game thread",
                       .get  = [] {
                           return control::Value{std::format(L"leader {}, stream {}, handed {}, returned {}, off-thread {}",
                                                             g_counts.leader, g_counts.stream, g_counts.handed, g_counts.returned,
                                                             g_counts.offThread)};
                       }});
    return true;
}

void Remove() noexcept
{
    g_return.Restore();
    g_hands[1].Restore();
    g_hands[0].Restore();
    g_stream.Restore();
    g_leaderToad.Restore();
    g_leader.Restore();
}

void Tick() noexcept
{
    if (g_placed || g_setLeader == 0)
    {
        return;
    }
    g_placed = true;
    g_thread = GetCurrentThreadId();
    // The entries open with pushes (position-independent); the game thread alone runs them, and it is here, outside.
    const bool placed = g_leader.Install(g_setLeader, std::span(facts::kSetPartyLeaderEntry.data(), facts::kSetPartyLeaderPushes),
                                         reinterpret_cast<std::uintptr_t>(&SetLeader)) &&
                        g_leaderToad.Install(g_setLeaderToad, std::span(facts::kSetPartyLeaderToadEntry.data(), facts::kSetPartyLeaderToadPushes),
                                             reinterpret_cast<std::uintptr_t>(&SetLeaderToad));
    if (!placed)
    {
        g_leaderToad.Restore();
        g_leader.Restore();
        platform::Log(L"party events: the leader funnel's entries could not be redirected; a leader change onto a further player's member is caught one tick late");
        return;
    }
    platform::Log(L"party events: installed; the game's leader changes, formation rebuilds and event hand-backs are heard");
}

}  // namespace lss::players::party_events
