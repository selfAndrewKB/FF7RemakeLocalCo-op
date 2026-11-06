#include "players/party_events.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/game_thread.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

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
using HandFn          = char (*)(void* unused);
using ReturnFn        = std::int64_t (*)(void* root);

// Set once at install.
Listener                g_listener;
std::uintptr_t          g_setLeader = 0, g_setLeaderToad = 0, g_hand = 0, g_return = 0;
platform::EntryRedirect g_leader, g_leaderToad, g_handEntry, g_returnEntry;
platform::CallRedirect  g_stream;
platform::GameThreadMark g_thread;
bool                    g_placed = false;

struct Counts
{
    std::uint32_t leader = 0, stream = 0, handed = 0, returned = 0, offThread = 0;
};
Counts g_counts;

bool OnGameThread() noexcept
{
    if (g_thread.Here())
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

// The hand reads no argument (it fetches the party manager itself); rcx is passed on untouched all the same.
char HandLeaderToAi(void* unused)
{
    const char result = reinterpret_cast<HandFn>(g_handEntry.Original())(unused);
    if (OnGameThread() && g_listener.leaderHandedToAi != nullptr)
    {
        ++g_counts.handed;
        g_listener.leaderHandedToAi(g_listener.context);
    }
    return result;
}

std::int64_t ReturnLeader(void* root)
{
    const std::int64_t result = reinterpret_cast<ReturnFn>(g_returnEntry.Original())(root);
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
    if (!setLeader || !setToad || !stream || !streamJmp || !hand || !ret)
    {
        platform::Log(L"party events: an engine check failed; the game's party funnels are not heard");
        return false;
    }
    g_listener      = listener;
    g_setLeader     = *setLeader;
    g_setLeaderToad = *setToad;
    g_hand          = *hand;
    g_return        = *ret;
    if (!g_stream.InstallTailJump(*streamJmp, *stream, reinterpret_cast<std::uintptr_t>(&FinishStream)))
    {
        platform::Log(L"party events: the formation rebuild's tail jump could not be redirected; the game's party funnels are not heard");
        return false;
    }
    control::Register({.name = L"party-events", .shape = control::Shape::Text,
                       .help = L"the game's party funnels heard: leader changes, formation rebuilds, the leader handed to its AI "
                               L"and returned (every site), calls off the game thread",
                       .get  = [] {
                           return control::Value{std::format(L"leader {}, stream {}, handed {}, returned {}, off-thread {}",
                                                             g_counts.leader, g_counts.stream, g_counts.handed, g_counts.returned,
                                                             g_counts.offThread)};
                       }});
    return true;
}

void Remove() noexcept
{
    g_returnEntry.Restore();
    g_handEntry.Restore();
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
    g_thread.Set();
    // The entries open position-independent (pushes, a home-area store, a frame reserve); the game thread alone
    // runs them, and it is here, outside. The hand's first two instructions go together: six bytes, as the pushes.
    const bool placed = g_leader.Install(g_setLeader, std::span(facts::kSetPartyLeaderEntry.data(), facts::kSetPartyLeaderPushes),
                                         reinterpret_cast<std::uintptr_t>(&SetLeader)) &&
                        g_leaderToad.Install(g_setLeaderToad, std::span(facts::kSetPartyLeaderToadEntry.data(), facts::kSetPartyLeaderToadPushes),
                                             reinterpret_cast<std::uintptr_t>(&SetLeaderToad)) &&
                        g_handEntry.Install(g_hand, std::span(facts::kHandLeaderToAiEntry.data(), facts::kHandLeaderToAiFirst),
                                            reinterpret_cast<std::uintptr_t>(&HandLeaderToAi)) &&
                        g_returnEntry.Install(g_return, std::span(facts::kReturnLeaderToPcEntry.data(), facts::kReturnLeaderToPcFirst),
                                              reinterpret_cast<std::uintptr_t>(&ReturnLeader));
    if (!placed)
    {
        g_returnEntry.Restore();
        g_handEntry.Restore();
        g_leaderToad.Restore();
        g_leader.Restore();
        platform::Log(L"party events: a funnel's entry could not be redirected; a leader change onto a further player's member is "
                      L"caught one tick late, and the game's hand-backs go unheard (the per-tick check alone)");
        return;
    }
    platform::Log(L"party events: installed; the game's leader changes, formation rebuilds, and its leader's hand to its AI and "
                  L"return are heard at every site");
}

}  // namespace lss::players::party_events
