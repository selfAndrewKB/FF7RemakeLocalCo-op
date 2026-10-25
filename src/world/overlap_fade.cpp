#include "world/overlap_fade.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include <windows.h>

namespace lss::world::overlap_fade {

namespace {

using platform::Read;
using platform::ReadPointer;

using StateCallFn = void (*)(void* state);

/// One move's state vtable +0x58 and the function the research read there.
struct Move
{
    const wchar_t*                 name;
    std::uintptr_t                 slot;
    const platform::FunctionFact*  fade;
};

constexpr std::array<Move, 8> kMoves{{
    {L"ladder", facts::kOverlapFadeLadderSlot, &facts::kOverlapFadeLadder},
    {L"crawl", facts::kOverlapFadeCrawlSlot, &facts::kOverlapFadeShared},
    {L"squeeze", facts::kOverlapFadeSqueezeSlot, &facts::kOverlapFadeShared},
    {L"balance beam", facts::kOverlapFadeBeamSlot, &facts::kOverlapFadeShared},
    {L"fan", facts::kOverlapFadeFanSlot, &facts::kOverlapFadeShared},
    {L"jump", facts::kOverlapFadeJumpSlot, &facts::kOverlapFadeJump},
    {L"trap", facts::kOverlapFadeTrapSlot, &facts::kOverlapFadeTrap},
    {L"vent crawl", facts::kOverlapFadeDuctSlot, &facts::kOverlapFadeDuct},
}};
constexpr std::size_t kMoveCount = kMoves.size();

/// What the mod did with one call while the fade is kept off.
enum class Choice : std::uint8_t
{
    KeptOff,  // the fade is off for this state: not called, so it never starts
    Clearing, // the fade was found on: the game's function, which can only fade it back in
};

std::array<platform::VTableEntry, kMoveCount> g_slots;
void**                                        g_partyManager = nullptr;
std::atomic<bool>                             g_keepOff{false};  // a further player is in and the switch is on
bool                                          g_enabled     = true;   // switch overlap-fade
bool                                          g_installed   = false;  // at least one slot placed
bool                                          g_wasKeptOff  = false;
std::atomic<std::uint64_t>                    g_leaderFades{0};      // the vent crawl's own rule hid player 1's character

struct Counts
{
    std::atomic<std::uint64_t> keptOff{0};
    std::atomic<std::uint64_t> clearing{0};
};
std::array<Counts, kMoveCount> g_counts;

/// The last choice per state, so a change is logged once (a handful of states act at a time); emptied whenever the
/// fade is kept off or allowed again, so a rejoin or a state at a reused address is logged afresh.
struct Seen
{
    const void* state  = nullptr;
    Choice      choice = Choice::KeptOff;
};
SRWLOCK             g_seenLock = SRWLOCK_INIT;  // never throws, unlike std::mutex in noexcept code
std::array<Seen, 8> g_seen{};
std::size_t         g_seenNext = 0;

/// Holds g_seenLock for a scope.
class SeenLock
{
public:
    SeenLock() noexcept { AcquireSRWLockExclusive(&g_seenLock); }
    ~SeenLock() { ReleaseSRWLockExclusive(&g_seenLock); }
    SeenLock(const SeenLock&)            = delete;
    SeenLock& operator=(const SeenLock&) = delete;
};

const wchar_t* ChoiceText(Choice choice) noexcept
{
    switch (choice)
    {
    case Choice::KeptOff:
        return L"kept off (the game's fade never starts)";
    case Choice::Clearing:
        return L"found on: the game's function fades it back in once nobody is close";
    }
    return L"?";
}

void Note(std::size_t move, const void* state, Choice choice) noexcept
{
    const SeenLock lock;
    for (Seen& seen : g_seen)
    {
        if (seen.state == state)
        {
            if (seen.choice != choice)
            {
                seen.choice = choice;
                platform::LogDebug(L"overlap fade: a {} state ({}) now {}", kMoves[move].name, state, ChoiceText(choice));
            }
            return;
        }
    }
    g_seen[g_seenNext] = {state, choice};
    g_seenNext         = (g_seenNext + 1) % g_seen.size();
    platform::LogDebug(L"overlap fade: a {} state ({}) {}", kMoves[move].name, state, ChoiceText(choice));
}

void ForgetSeen() noexcept
{
    const SeenLock lock;
    g_seen.fill({});
    g_seenNext = 0;
}

std::uint8_t NotFaded(const void* state) noexcept
{
    std::uint8_t notFaded = 0xFF;
    Read(static_cast<const std::uint8_t*>(state) + facts::kStateNotFaded, notFaded);
    return notFaded;
}

/// True if the state's character is the party leader's type (the game's own test, read the same way). Read live: the
/// post-tick runs outside every scope of the mod, so it is player 1's leader; if a change ever runs it inside a seat's
/// scopes (as the slide-down's), revisit this test (research §4.2 E12).
bool LeaderState(const void* state) noexcept
{
    const void*  component = ReadPointer(state, facts::kStateComponent);
    void*        manager   = nullptr;
    std::uint8_t mine = 0xFF, leader = 0xFE;
    return component != nullptr && Read(g_partyManager, manager) && manager != nullptr &&
           Read(static_cast<const std::uint8_t*>(component) + facts::kFieldActionCompPlayerType, mine) &&
           Read(static_cast<const std::uint8_t*>(manager) + facts::kPartyLeaderType, leader) && mine == leader;
}

template <std::size_t I>
void KeepFadeOff(void* state)
{
    const auto original = reinterpret_cast<StateCallFn>(g_slots[I].Original());
    if (!g_keepOff.load(std::memory_order_relaxed))
    {
        original(state);
        return;
    }
    const std::uint8_t notFaded = NotFaded(state);
    platform::LogTrace(L"overlap fade: {} state {} not faded {}", kMoves[I].name, state, notFaded);
    if (LeaderState(state))
    {
        original(state);  // the leader's own branch: only the vent crawl has one (when it fires is unknown, research §2.4)
        if (notFaded != 0 && NotFaded(state) == 0)
        {
            g_leaderFades.fetch_add(1, std::memory_order_relaxed);
            platform::LogDebug(L"overlap fade: player 1's character hidden by the game's own {} rule (its leader branch, left as the "
                               L"game's)", kMoves[I].name);
        }
        return;
    }
    if (notFaded != 0 && notFaded != 0xFF)
    {
        g_counts[I].keptOff.fetch_add(1, std::memory_order_relaxed);
        Note(I, state, Choice::KeptOff);
        return;
    }
    g_counts[I].clearing.fetch_add(1, std::memory_order_relaxed);
    Note(I, state, Choice::Clearing);
    original(state);
}

template <std::size_t... I>
constexpr std::array<StateCallFn, kMoveCount> MakeThunks(std::index_sequence<I...>)
{
    return {&KeepFadeOff<I>...};
}
constexpr std::array<StateCallFn, kMoveCount> kThunks = MakeThunks(std::make_index_sequence<kMoveCount>{});

void Apply(bool keepOff) noexcept
{
    g_keepOff.store(keepOff, std::memory_order_relaxed);
    if (keepOff != g_wasKeptOff)
    {
        g_wasKeptOff = keepOff;
        ForgetSeen();
        if (keepOff)
        {
            platform::Log(L"overlap fade: kept off while a further player is in (no character turns see-through in a ladder, "
                          L"squeeze, crawl or the like)");
        }
        else
        {
            platform::Log(L"overlap fade: the game's again (characters behind another in those moves turn see-through as in "
                          L"single player)");
        }
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    if (!manager)
    {
        platform::LogWarning(L"overlap fade: the party manager check failed; characters turn see-through in those moves as the "
                             L"game decides");
        return false;
    }
    g_partyManager = reinterpret_cast<void**>(*manager);
    std::wstring placed, refused;
    for (std::size_t i = 0; i < kMoveCount; ++i)
    {
        const auto slot = image.Rebase(kMoves[i].slot, 8);
        const auto fade = platform::FindFunction(image, *kMoves[i].fade);
        const bool ok   = slot && fade && g_slots[i].Replace(*slot, *fade, reinterpret_cast<std::uintptr_t>(kThunks[i]));
        (ok ? placed : refused) += std::format(L"{}{}", (ok ? placed : refused).empty() ? L"" : L", ", kMoves[i].name);
    }
    if (placed.empty())
    {
        platform::LogWarning(L"overlap fade: no slot held the researched function; characters turn see-through in those moves as "
                             L"the game decides");
        return false;
    }
    g_installed = true;
    if (!refused.empty())
    {
        platform::LogWarning(L"overlap fade: the game's fade kept for {} (its slot held another function)", refused);
    }
    control::Register({.name  = L"overlap-fade",
                       .shape = control::Shape::OnOff,
                       .help  = L"no character turns see-through in a ladder, squeeze, crawl or the like while a further player is "
                                L"in (world/overlap_fade.h; on); off: the game's fade",
                       .set   = [](const control::Value& value) {
                           g_enabled = std::get<bool>(value);
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"world.overlap-fade", .shape = control::Shape::Text,
                       .help = L"(read only) whether the overlap fade is kept off now, and per move the calls kept off and the "
                               L"fades found on and left to clear",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"overlap fade: installed ({}); while a further player is in, no character turns see-through in those moves",
                  placed);
    return true;
}

void Remove() noexcept
{
    g_keepOff.store(false, std::memory_order_relaxed);
    for (auto& slot : g_slots)
    {
        slot.Restore();
    }
}

void Tick(int seatsIn) noexcept
{
    if (g_installed)
    {
        Apply(g_enabled && seatsIn > 0);
    }
}

std::wstring Reading() noexcept
try
{
    std::wstring out = std::format(L"{} (switch {})", g_keepOff.load(std::memory_order_relaxed) ? L"kept off" : L"the game's",
                                   g_enabled ? L"on" : L"off");
    out += std::format(L"; player 1 hidden by the vent crawl's own rule {}", g_leaderFades.load(std::memory_order_relaxed));
    for (std::size_t i = 0; i < kMoveCount; ++i)
    {
        out += std::format(L"; {}: {}{} kept off, {} found on", kMoves[i].name, g_slots[i].Active() ? L"" : L"(not placed) ",
                           g_counts[i].keptOff.load(std::memory_order_relaxed),
                           g_counts[i].clearing.load(std::memory_order_relaxed));
    }
    return out;
}
catch (...)
{
    return L"unread";
}

}  // namespace lss::world::overlap_fade
