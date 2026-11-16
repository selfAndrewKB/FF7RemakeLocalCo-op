#include "players/event_move_hand.h"

#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/seat_camera.h"
#include "players/seats.h"
#include "players/session.h"
#include "world/named_actor.h"

#include <atomic>
#include <cstdint>

namespace lss::players::event_move_hand {

namespace {

using WriteFn = char (*)(std::uint64_t actor, std::int64_t unused, void* params);

platform::EntryRedirect g_redirect;
std::uintptr_t          g_at        = 0;
Session*                g_session   = nullptr;
bool                    g_pending   = false;
bool                    g_installed = false;
std::atomic<bool>       g_placed{false};

/// Hands the named character to its AI before the game's write when it is a seat's member its player holds (found as
/// the writer finds it; a dying one is left to the writer, which refuses it).
void HandIfHeld(std::uint64_t actor) noexcept
{
    void* const character = g_session != nullptr ? world::named_actor::Find(actor) : nullptr;
    if (character == nullptr)
    {
        return;
    }
    if (const int seat = g_session->HandBeforeScriptMove(character); seat != 0)
    {
        platform::LogDebug(L"event move: {}'s member went to its AI before the level's scripted move was written on it (as in "
                           L"single player, where its AI holds it then)",
                           SeatName(seat));
    }
}

char Write(std::uint64_t actor, std::int64_t unused, void* params)
{
    if (g_placed.load(std::memory_order_relaxed) && seat_camera::IsGameThread())
    {
        HandIfHeld(actor);
    }
    return reinterpret_cast<WriteFn>(g_redirect.Original())(actor, unused, params);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto at = image.Rebase(facts::kEventMoveWrite, 16);
    if (!at || !image.BytesMatch(facts::kEventMoveWrite, facts::kEventMoveWriteEntry))
    {
        platform::Log(L"event move: the level's scripted-move writer is not what the research read; a further player's member "
                      L"goes to its AI after the write, as before (a one-shot animation on it can end at once)");
        return false;
    }
    g_at        = *at;
    g_pending   = true;  // placed by the game thread's tick, outside the world tick
    g_installed = true;
    return true;
}

void SetSession(Session* session) noexcept
{
    g_session = session;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_placed.store(false);
    if (!g_redirect.Restore())
    {
        platform::Log(L"event move: the writer's entry was changed after us; left in place (it passes everything while off)");
    }
}

void Tick() noexcept
{
    if (!g_pending)
    {
        return;
    }
    g_pending     = false;
    const bool ok = g_redirect.Install(g_at, facts::kEventMoveWriteFirst, reinterpret_cast<std::uintptr_t>(&Write));
    g_placed.store(ok);
    platform::Log(L"event move: {}", ok ? L"a further player's held member goes to its AI before the level writes a scripted move "
                                          L"on it (its one-shot animations play to their end)"
                                        : L"the writer could not be redirected; a further player's member goes to its AI after "
                                          L"the write, as before");
}

}  // namespace lss::players::event_move_hand
