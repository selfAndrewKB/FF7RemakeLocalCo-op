#include "players/ai_move_guard.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/seats.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>

namespace lss::players::ai_move_guard {

namespace {

using CanRequestFn = bool (*)(void* model, std::uint8_t slot, std::uint8_t priority);

constexpr std::uint8_t  kMoveSlot      = 0;
constexpr std::uint32_t kLogEveryTicks = 600;  // the refusals' Debug line, when there were any

platform::EntryRedirect g_redirect;
std::uintptr_t          g_at        = 0;
bool                    g_pending   = false;
bool                    g_installed = false;
std::atomic<bool>       g_on{true};
std::atomic<bool>       g_placed{false};

/// Each seat's member while its player is in, published by the game thread for the replacement on any thread.
std::array<std::atomic<const void*>, kSeatsBuilt + 1> g_members{};
/// Refusals per seat since the last line (the replacement counts, the game thread logs).
std::array<std::atomic<std::uint32_t>, kSeatsBuilt + 1> g_refused{};
std::array<std::uint32_t, kSeatsBuilt + 1>              g_refusedTotal{};
std::uint32_t                                           g_ticks = 0;

/// The seat whose member `character` is while that member's PLAYER slot holds a controller, else 0.
int HeldBySeat(const std::uint8_t* character) noexcept
{
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (g_members[static_cast<std::size_t>(seat)].load(std::memory_order_acquire) != character)
        {
            continue;
        }
        void* controller = nullptr;
        return platform::Read(character + facts::kCharacterPlayerSlot, controller) && controller != nullptr ? seat : 0;
    }
    return 0;
}

bool CanRequest(void* model, std::uint8_t slot, std::uint8_t priority)
{
    if (slot == kMoveSlot && model != nullptr && g_on.load(std::memory_order_relaxed))
    {
        const auto* character = static_cast<const std::uint8_t*>(model) - facts::kCharacterAiModel;
        if (const int seat = HeldBySeat(character); seat != 0)
        {
            g_refused[static_cast<std::size_t>(seat)].fetch_add(1, std::memory_order_relaxed);
            return false;  // the game's own answer while manual control holds the move slot
        }
    }
    return reinterpret_cast<CanRequestFn>(g_redirect.Original())(model, slot, priority);
}

std::wstring Reading()
{
    std::wstring text = std::format(L"guard {}{}", g_placed.load() ? L"placed" : L"not placed", g_on.load() ? L"" : L" (switched off)");
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const auto i = static_cast<std::size_t>(seat);
        text += std::format(L"; {}: {} move request(s) refused while held", SeatName(seat),
                            g_refusedTotal[i] + g_refused[i].load(std::memory_order_relaxed));
    }
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto at = image.Rebase(facts::kAiModelCanRequestBehavior, 16);
    if (!at || !image.BytesMatch(facts::kAiModelCanRequestBehavior, facts::kAiModelCanRequestBehaviorFirst))
    {
        platform::Log(L"AI move guard: the AI model's behaviour check is not what the research read; a further player's member "
                      L"goes to its AI for an interest walk, as before");
        return false;
    }
    g_at        = *at;
    g_pending   = true;  // placed by the game thread's tick, outside the world tick
    g_installed = true;
    control::Register({.name  = L"ai-move-guard",
                       .shape = control::Shape::OnOff,
                       .help  = L"a further player's held member is refused the AI model's move behaviours (an interest walk, a "
                                L"bump), as manual control refuses them; off: such a walk hands the member to its AI, as before "
                                L"(on at start)",
                       .set   = [](const control::Value& value) {
                           g_on.store(std::get<bool>(value));
                           platform::Log(L"AI move guard: {}", g_on.load() ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_on.load()}; }});
    control::Register({.name  = L"players.ai-move-guard",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) whether the AI move guard is placed, and the move requests it refused per player",
                       .get   = [] {
                           try
                           {
                               return control::Value{Reading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
    return true;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_on.store(false);
    g_placed.store(false);
    if (!g_redirect.Restore())
    {
        platform::Log(L"AI move guard: the behaviour check's entry was changed after us; left in place (it passes everything while off)");
    }
}

void Tick() noexcept
{
    if (g_pending)
    {
        g_pending     = false;
        const bool ok = g_redirect.Install(g_at, facts::kAiModelCanRequestBehaviorFirst, reinterpret_cast<std::uintptr_t>(&CanRequest));
        g_placed.store(ok);
        platform::Log(L"AI move guard: {}", ok ? L"a further player's held member is refused the AI model's move behaviours (an interest "
                                                 L"walk stays with its player; ai-move-guard on)"
                                               : L"the behaviour check could not be redirected; an interest walk hands the member to its "
                                                 L"AI, as before");
    }
    if (++g_ticks % kLogEveryTicks != 0)
    {
        return;
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const auto          i     = static_cast<std::size_t>(seat);
        const std::uint32_t count = g_refused[i].exchange(0, std::memory_order_relaxed);
        if (count != 0)
        {
            g_refusedTotal[i] += count;
            platform::LogDebug(L"AI move guard: {} AI move request(s) refused for {}'s member in the last {} ticks (the game's own "
                               L"answer under manual control)",
                               count, SeatName(seat), kLogEveryTicks);
        }
    }
}

void TickSeat(int seat, void* member) noexcept
{
    if (seat >= 1 && seat <= kSeatsBuilt)
    {
        g_members[static_cast<std::size_t>(seat)].store(member, std::memory_order_release);
    }
}

bool Active() noexcept
{
    return g_placed.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed);
}

}  // namespace lss::players::ai_move_guard
