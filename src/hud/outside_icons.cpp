#include "hud/outside_icons.h"

#include "battle/target_cursor.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "hud/arrow_timers.h"
#include "hud/overlay_pass.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/scopes.h"
#include "players/seats.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace lss::hud::outside_icons {

namespace {

using platform::Read;
using TickFn      = void (*)(void* manager);
using ContextPcFn = void* (*)(const void* context);

// Set at install, then only read.
TickFn         g_tick      = nullptr;
std::uintptr_t g_contextAt = 0;  // FLocalPlayerContext::GetPlayerController
std::array<std::uintptr_t, facts::kOutsideIconContextCalls.size()> g_sites{};
void**         g_slot      = nullptr;  // &EndUISystem[38]
std::uint8_t** g_gameMode  = nullptr;  // &g_AEndGameMode

// Game thread.
static_assert(players::kSeatsBuilt < players::scopes::kMaxPlayers);
std::array<platform::CallRedirect, facts::kOutsideIconContextCalls.size()> g_lookups;
DWORD g_gameThread = 0;
bool  g_pending  = false;
bool  g_ready    = false;  // the lookups are ours
bool  g_refused  = false;  // a run faulted: the game's arrows only, from then on
bool  g_on       = true;
void* g_override = nullptr;  // what the tick's controller lookups answer during a seat's run
std::array<arrow_timers::SeatTimers, players::scopes::kMaxPlayers> g_timers;  // index = seat ([0] unused)
std::array<bool, players::scopes::kMaxPlayers> g_ran{};                       // the seat's column ran this frame
std::array<bool, players::scopes::kMaxPlayers> g_told{};                      // its first run logged since its last stop
bool g_furtherRan = false;

struct Seen
{
    std::array<std::uint32_t, players::scopes::kMaxPlayers> runs{};
    std::uint32_t restores     = 0;
    std::uint32_t answered     = 0;  // controller lookups answered with a seat's controller
    std::uint32_t countChanged = 0;  // runs after which the arrow count differed (the game's own churn, or ours)
    std::uint32_t noController = 0, noGameMode = 0, tooMany = 0;
} g_seen;

/// A run that could not finish as designed: every view shows the game's arrows from then on (logged once).
void StandAside(const wchar_t* what) noexcept
{
    if (!g_refused)
    {
        g_refused = true;
        platform::LogWarning(L"HUD arrows: {} failed; the per-view arrows stood aside (every view shows player 1's arrows)", what);
    }
}

/// The tick's two controller lookups.
void* Lookup(const void* context)
{
    if (g_override != nullptr)
    {
        ++g_seen.answered;
        return g_override;
    }
    return reinterpret_cast<ContextPcFn>(g_contextAt)(context);
}

/// The arrow manager, if it exists and its gate is open (closed outside battle: the tick then does nothing).
std::uint8_t* Manager() noexcept
{
    std::uint8_t* manager = nullptr;
    std::uint8_t  gate    = 0;
    if (g_slot == nullptr || !Read(g_slot, manager) || manager == nullptr || !Read(manager + facts::kOutsideIconGate, gate) ||
        gate == 0)
    {
        return nullptr;
    }
    return manager;
}

std::uint8_t* GameMode() noexcept
{
    std::uint8_t* mode = nullptr;
    return g_gameMode != nullptr && Read(g_gameMode, mode) ? mode : nullptr;
}

struct Arrow
{
    std::uint8_t*     element = nullptr;
    arrow_timers::Key key;
};
struct Arrows
{
    std::array<Arrow, arrow_timers::kCapacity> items{};
    std::size_t                                count = 0;
};

enum class Walk
{
    Read,
    Unreadable,
    TooMany,  // more live arrows than a seat's store holds
};

/// The manager's live arrows (the set's allocated elements).
Walk LiveArrows(const std::uint8_t* manager, Arrows& out) noexcept
{
    out.count   = 0;
    Walk result = Walk::Read;
    const bool read =
        platform::ForEachSparseElement(reinterpret_cast<std::uintptr_t>(manager) + facts::kOutsideIconSetData, facts::kOutsideIconStride,
                                       platform::kSmallSetMaxBits, [&](std::uintptr_t address) {
                                           if (out.count == out.items.size())
                                           {
                                               result = Walk::TooMany;
                                               return false;
                                           }
                                           auto*  element = reinterpret_cast<std::uint8_t*>(address);
                                           Arrow& arrow   = out.items[out.count];
                                           void*  source  = nullptr;
                                           if (!Read(element, arrow.key.widget) || !Read(element + facts::kOutsideIconSource, source))
                                           {
                                               result = Walk::Unreadable;
                                               return false;
                                           }
                                           arrow.element    = element;
                                           arrow.key.source = reinterpret_cast<std::uintptr_t>(source);
                                           ++out.count;
                                           return true;
                                       });
    return read ? result : Walk::Unreadable;
}

float TimerOf(const Arrow& arrow) noexcept
{
    float t = 0.0f;
    Read(arrow.element + facts::kOutsideIconTimer, t);
    return t;
}

void SetTimer(const Arrow& arrow, float t) noexcept
{
    platform::WriteGuarded(arrow.element + facts::kOutsideIconTimer, &t, sizeof(t));
}

std::wstring Reading()
{
    std::wstring text = std::format(L"{}; runs", !g_on ? L"off" : g_ready ? L"on" : L"not placed (player 1's arrows in every view)");
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        text += std::format(L" {} {} ({} arrows timed)", players::SeatName(seat), g_seen.runs[static_cast<std::size_t>(seat)],
                            g_timers[static_cast<std::size_t>(seat)].Count());
    }
    text += std::format(L"; player 1's restored {}; lookups answered {}; runs that changed the arrow count {}; skipped: no "
                        L"controller {}, no game mode {}, more arrows than a store holds {}{}",
                        g_seen.restores, g_seen.answered, g_seen.countChanged, g_seen.noController, g_seen.noGameMode,
                        g_seen.tooMany, g_refused ? L"; STOOD ASIDE after a failure" : L"");
    return text;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tick     = platform::FindFunction(image, facts::kOutsideIconTick);
    const auto context  = image.Rebase(facts::kPlayerContextController, 16);
    const auto slot     = image.Rebase(facts::kOutsideIconSlotGlobal, sizeof(void*));
    const auto gameMode = image.Rebase(facts::kGameModeGlobal, sizeof(void*));
    bool       sites    = true;
    for (std::size_t i = 0; i < g_sites.size(); ++i)
    {
        const auto site = image.Rebase(facts::kOutsideIconContextCalls[i], 5);
        g_sites[i]      = site ? *site : 0;
        sites           = sites && site.has_value();
    }
    if (!tick || !context || !slot || !gameMode || !sites ||
        !image.BytesMatch(facts::kPlayerContextController, facts::kPlayerContextControllerFirst))
    {
        platform::LogWarning(L"HUD arrows: the off-screen arrows' tick is not what the research read; every view shows player 1's arrows");
        return false;
    }
    g_tick      = reinterpret_cast<TickFn>(*tick);
    g_contextAt = *context;
    g_slot      = reinterpret_cast<void**>(*slot);
    g_gameMode  = reinterpret_cast<std::uint8_t**>(*gameMode);
    g_pending   = true;  // placed by the game thread's tick, outside the arrow tick (one call crosses a block)
    control::Register({.name  = L"hud.outside-icons",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the off-screen enemy arrows per view: each player's runs of the game's arrow tick, "
                                L"the arrows each keeps a timer for, player 1's restores, the controller lookups answered",
                       .get   = [] { return control::Value{Reading()}; }});
    control::Register({.name  = L"hud-outside-icons",
                       .shape = control::Shape::OnOff,
                       .help  = L"each player's off-screen enemy arrows placed through that player's camera (hud/outside_icons.h; on)",
                       .set   = [](const control::Value& value) {
                           g_on = std::get<bool>(value);
                           platform::Log(L"HUD arrows {}", g_on ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_on}; }});
    return true;
}

void Remove() noexcept
{
    g_ready = false;
    g_lookups[1].Restore();
    // The first lookup's call crosses a block: put back only from the game thread (outside the tick). Elsewhere it stays
    // ours, harmless: the DLL is pinned and the lookup answers the game's own controller while no run is on.
    if (GetCurrentThreadId() == g_gameThread)
    {
        g_lookups[0].Restore();
    }
    else if (g_lookups[0].IsInstalled())
    {
        platform::Log(L"HUD arrows: the first controller lookup left redirected (removed off the game thread; it answers the game's own)");
    }
}

void Tick() noexcept
{
    g_gameThread = GetCurrentThreadId();
    if (!g_pending)
    {
        return;
    }
    g_pending = false;
    const auto replacement = reinterpret_cast<std::uintptr_t>(&Lookup);
    g_ready = g_lookups[0].InstallFromOwnThread(g_sites[0], g_contextAt, replacement) &&
              g_lookups[1].Install(g_sites[1], g_contextAt, replacement);
    if (!g_ready)
    {
        Remove();  // on the game thread: both put back
        platform::LogWarning(L"HUD arrows: the arrow tick's controller lookups could not be redirected; every view shows player 1's arrows");
        return;
    }
    platform::Log(L"HUD arrows: each player's off-screen enemy arrows placed through that player's camera (hud-outside-icons on)");
}

void RunForColumn(int column, void* controller) noexcept
{
    const int seat = overlay_pass::SeatOf(column);
    if (!g_on || !g_ready || g_refused || seat < 1 || seat > players::kSeatsBuilt)
    {
        return;
    }
    std::uint8_t* manager = Manager();
    if (manager == nullptr)
    {
        return;  // no battle: nothing to place, and the seat's timers are forgotten at the frame's end
    }
    if (controller == nullptr)
    {
        ++g_seen.noController;
        return;
    }
    if (GameMode() == nullptr)
    {
        ++g_seen.noGameMode;  // Place would step by its 0.1 s default
        return;
    }
    std::uint64_t liveName = 0, seatName = 0;
    Arrows        before;
    const Walk    walk = LiveArrows(manager, before);
    if (walk == Walk::TooMany)
    {
        ++g_seen.tooMany;  // the seat's run would step player 1's timers of the arrows past the store: not run
        return;
    }
    if (walk != Walk::Read || !Read(manager + facts::kOutsideIconHighlightName, liveName))
    {
        StandAside(L"reading the arrows");
        return;
    }
    battle::target_cursor::HighlightName(seat, seatName);  // 0 before the seat's first target, as a new manager
    auto& timers = g_timers[static_cast<std::size_t>(seat)];
    timers.BeginIn();
    for (std::size_t i = 0; i < before.count; ++i)
    {
        SetTimer(before.items[i], timers.In(before.items[i].key, TimerOf(before.items[i])));
    }
    platform::WriteGuarded(manager + facts::kOutsideIconHighlightName, &seatName, sizeof(seatName));
    g_override    = controller;
    const bool ok = platform::CallGuarded(g_tick, manager);
    g_override    = nullptr;
    platform::WriteGuarded(manager + facts::kOutsideIconHighlightName, &liveName, sizeof(liveName));
    Arrows after;
    if (LiveArrows(manager, after) != Walk::Read)
    {
        after.count = 0;  // the seat's timers are dropped below; player 1's stay as the run left them, so stand aside
        StandAside(L"reading the arrows after a player's run");
    }
    timers.BeginOut();
    for (std::size_t i = 0; i < after.count; ++i)
    {
        if (const auto mine = timers.Out(after.items[i].key, TimerOf(after.items[i])))
        {
            SetTimer(after.items[i], *mine);
        }
    }
    timers.EndOut();
    if (after.count != before.count && g_seen.countChanged++ == 0)
    {
        platform::LogDebug(L"HUD arrows: a run by {} changed the arrow count ({} -> {}); counted in hud.outside-icons",
                           players::SeatName(seat), before.count, after.count);
    }
    if (!ok)
    {
        StandAside(L"the arrow tick in a player's pass");
    }
    ++g_seen.runs[seat];
    g_ran[static_cast<std::size_t>(seat)] = true;
    g_furtherRan                          = true;
    if (!g_told[static_cast<std::size_t>(seat)])
    {
        g_told[static_cast<std::size_t>(seat)] = true;
        platform::LogDebug(L"HUD arrows: {}'s arrows placed through that player's camera (column {}, {} arrows, highlight {})",
                           players::SeatName(seat), column + 1, after.count,
                           seatName != 0 ? L"its last target" : L"none yet");
    }
}

void RestorePlayerOne() noexcept
{
    if (!g_furtherRan)
    {
        return;
    }
    std::uint8_t* manager = Manager();
    std::uint8_t* mode    = GameMode();
    float         step    = 0.0f;
    if (manager == nullptr || mode == nullptr || !Read(mode + facts::kGameModeHudStep, step))
    {
        return;
    }
    // With the step at 0, an arrow on player 1's screen keeps its timer and one off it gets the full value the tick
    // left: every input is the tick's, so the placement is the tick's.
    const float zero = 0.0f;
    if (!platform::WriteGuarded(mode + facts::kGameModeHudStep, &zero, sizeof(zero)))
    {
        StandAside(L"holding the HUD step for player 1's pass");
        return;
    }
    const bool ok = platform::CallGuarded(g_tick, manager);
    platform::WriteGuarded(mode + facts::kGameModeHudStep, &step, sizeof(step));
    if (ok)
    {
        ++g_seen.restores;
    }
    else
    {
        StandAside(L"the arrow tick in player 1's pass");
    }
}

void EndFrame() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto index = static_cast<std::size_t>(seat);
        if (!g_ran[index])
        {
            g_timers[index].Forget();  // out, a mirror's column, or no battle: its arrows start again at 0
            g_told[index] = false;
        }
        g_ran[index] = false;
    }
    g_furtherRan = false;
}

}  // namespace lss::hud::outside_icons
