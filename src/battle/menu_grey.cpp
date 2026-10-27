#include "battle/menu_grey.h"

#include "battle/command_menu_tick.h"
#include "battle/command_menu.h"
#include "battle/menu_grey_rules.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>

#include <intrin.h>

namespace lss::battle::menu_grey {

namespace {

using platform::Read;

using ActivateFn   = void (*)(void* filters, std::uint8_t type, std::uint8_t instant);
using DeactivateFn = void (*)(void* filters, std::uint8_t type);

// Set at install, then only read.
std::uintptr_t g_filtersGlobal = 0;  // EndUISystem[37]: the UI filter manager
std::uintptr_t g_commandGlobal = 0;  // EndUISystem[19]: player 1's command controller
ActivateFn     g_activate      = nullptr;
DeactivateFn   g_deactivate    = nullptr;

platform::CallRedirect                                            g_open;      // the wait's spawn
std::array<platform::CallRedirect, facts::kUiFilterRestoreCalls.size()> g_restores;  // after another menu closes

// Game thread.
std::uint32_t g_opensSkipped = 0, g_restoresSkipped = 0, g_flipStops = 0, g_flipStarts = 0, g_settled = 0;
void*         g_playerOne = nullptr;  // player 1's command controller, noted at the engine tick (outside every scope)
bool          g_debt      = false;    // a skip or a stop may have left another owner's count with no effect
bool          g_slows     = true;     // the slow-down switch as last seen at the tick

/// Filter 0's effect handle (-1: none), shared count and suppressed byte.
struct FilterState
{
    std::int32_t handle = -1, count = 0;
    std::uint8_t suppressed = 0;
};
bool ReadFilter(const std::uint8_t* filters, FilterState& out) noexcept
{
    return filters != nullptr && Read(filters + facts::kUiFilterHandles, out.handle) &&
           Read(filters + facts::kUiFilterCounts, out.count) && Read(filters + facts::kUiFilterSuppressed, out.suppressed);
}

/// The filter as the rules see it.
menu_grey_rules::Filter Rules(const FilterState& state) noexcept
{
    return {.effectUp = state.handle != -1, .count = state.count, .suppressed = state.suppressed != 0};
}

void* Filters() noexcept
{
    void* filters = nullptr;
    return Read(reinterpret_cast<const void*>(g_filtersGlobal), filters) ? filters : nullptr;
}

/// The controller's wait state is opening or open (1 or 5: LatchRef's 0 -> 1 -> 5 -> 4 -> 0; an i32, its low byte read).
bool Waits(const void* controller) noexcept
{
    std::uint8_t state = 0;
    return controller != nullptr &&
           Read(static_cast<const std::uint8_t*>(controller) + facts::kCommandCtrlWaitState, state) &&
           (state == facts::kCommandWaitOpening || state == facts::kCommandWaitOpen);
}

/// Player 1's or any further player's battle menu waits. Inside a seat's UI slot scope EndUISystem[19] holds the
/// seat's controller: player 1's is then the one noted at the engine tick.
bool BattleMenuWaits() noexcept
{
    void* playerOne = nullptr;
    if (players::scopes::OpenPlayer(players::scopes::UiSlot) != 0)
    {
        playerOne = g_playerOne;  // a plain game object (no UObject): read guarded, noted again every engine tick
    }
    else
    {
        Read(reinterpret_cast<const void*>(g_commandGlobal), playerOne);
    }
    if (Waits(playerOne))
    {
        return true;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (command_menu::Ready(seat) && Waits(command_menu::Controller(seat)))
        {
            return true;
        }
    }
    return false;
}

/// The wait's spawn of filter 0 (FieldCommandCtrl__WaitModeFilter_LatchRef, the count's 0 -> 1).
void Open(void* filters, std::uint8_t type, std::uint8_t instant)
{
    const menu_grey_rules::Act act = menu_grey_rules::AtOpen(command_menu_tick::MenuSlow());
    if (act == menu_grey_rules::Act::Pass)
    {
        g_activate(filters, type, instant);
        return;
    }
    ++g_opensSkipped;
    g_debt = g_debt || menu_grey_rules::Owes(act);
    const int seat = command_menu_tick::Ticking() ? command_menu_tick::TickingSeat() : 0;
    platform::LogDebug(L"menu grey: {}'s battle menu opened; its grey is skipped (the slow-down is switched off)",
                       players::SeatName(seat));
}

/// A menu closing over the filters brings each counted one back; filter 0 not while a battle menu waits, switched off.
void RestoreAfterMenu(void* filters, std::uint8_t type, std::uint8_t instant)
{
    // The wait test only when it can matter (the other filters and the switch on pass whatever it says).
    const bool                 slows = command_menu_tick::MenuSlow();
    const bool                 wait  = type == facts::kUiFilterWait;
    const menu_grey_rules::Act act   = menu_grey_rules::AtRestore(slows, wait, wait && !slows && BattleMenuWaits());
    if (act == menu_grey_rules::Act::Skip)
    {
        ++g_restoresSkipped;
        g_debt = g_debt || menu_grey_rules::Owes(act);
        const auto here = platform::IdaAddress(reinterpret_cast<std::uintptr_t>(_ReturnAddress()));  // as IDA and the research name it
        platform::LogDebug(L"menu grey: a menu closed over a waiting battle menu (the call returning to {:#x}); its grey is "
                           L"not brought back (the slow-down is switched off)",
                           here);
        return;
    }
    g_activate(filters, type, instant);
}

/// The slow-down was switched since the last tick (game thread, outside every scope): a waiting battle menu's grey
/// follows.
void OnSwitched(bool slows) noexcept
{
    auto* const filters = static_cast<std::uint8_t*>(Filters());
    FilterState state;
    if (!g_open.IsInstalled() || !ReadFilter(filters, state))
    {
        return;
    }
    const menu_grey_rules::Filter filter = Rules(state);
    const menu_grey_rules::Act    act    = menu_grey_rules::AtSwitch(slows, filter, !slows && filter.effectUp && BattleMenuWaits());
    if (menu_grey_rules::Owes(act))
    {
        g_debt = true;
        if (platform::CallGuarded(g_deactivate, static_cast<void*>(filters), facts::kUiFilterWait))
        {
            ++g_flipStops;
            platform::LogDebug(L"menu grey: a waiting battle menu's grey stopped (the slow-down switched off)");
        }
        else
        {
            platform::LogWarning(L"menu grey: the game's stop of the grey raised; the grey may stay until the menu closes");
        }
    }
    else if (menu_grey_rules::Pays(act))
    {
        g_debt = false;  // as the game's own unsuppress brings a counted filter back
        if (platform::CallGuarded(g_activate, static_cast<void*>(filters), facts::kUiFilterWait, std::uint8_t{0}))
        {
            ++g_flipStarts;
            platform::LogDebug(L"menu grey: the counted grey started again (the slow-down switched on)");
        }
        else
        {
            platform::LogWarning(L"menu grey: the game's start of the grey raised");
        }
    }
}

std::wstring Reading()
{
    FilterState state;
    const bool  read = ReadFilter(static_cast<const std::uint8_t*>(Filters()), state);
    return std::format(L"{}; the wait's grey {}; spawns skipped {}, restores skipped {}, stopped at a switch {}, started at a "
                       L"switch {}, debts settled {} (one waiting: {}); filter 0 now: {}",
                       g_open.IsInstalled() ? L"on" : L"off", command_menu_tick::MenuSlow() ? L"follows the game" : L"skipped",
                       g_opensSkipped, g_restoresSkipped, g_flipStops, g_flipStarts, g_settled, g_debt ? L"yes" : L"no",
                       read ? std::format(L"effect {}, count {}, suppressed {}", state.handle == -1 ? L"none" : L"up",
                                          state.count, state.suppressed)
                            : std::wstring(L"unread"));
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto filters    = image.Rebase(facts::kUiFilterSlotGlobal, sizeof(void*));
    const auto command    = image.Rebase(facts::kCommandCtrlSlotGlobal, sizeof(void*));
    const auto activate   = platform::FindFunction(image, facts::kUiFilterActivate);
    const auto deactivate = platform::FindFunction(image, facts::kUiFilterDeactivate);
    const auto openSite   = image.Rebase(facts::kUiFilterWaitOpenCall, 5);
    if (!filters || !command || !activate || !deactivate || !openSite)
    {
        platform::Log(L"menu grey: an engine check failed; an open battle menu greys the screen even with the slow-down off");
        return false;
    }
    g_filtersGlobal = *filters;
    g_commandGlobal = *command;
    g_activate      = reinterpret_cast<ActivateFn>(*activate);
    g_deactivate    = reinterpret_cast<DeactivateFn>(*deactivate);
    if (!g_open.Install(*openSite, *activate, reinterpret_cast<std::uintptr_t>(&Open)))
    {
        platform::Log(L"menu grey: the wait's grey call could not be redirected; an open battle menu greys the screen even "
                      L"with the slow-down off");
        return false;
    }
    std::uint32_t restores = 0;
    for (std::size_t i = 0; i < g_restores.size(); ++i)
    {
        const auto site = image.Rebase(facts::kUiFilterRestoreCalls[i], 5);
        restores += site && g_restores[i].Install(*site, *activate, reinterpret_cast<std::uintptr_t>(&RestoreAfterMenu)) ? 1 : 0;
    }
    g_slows = command_menu_tick::MenuSlow();
    control::Register({.name  = L"menu-grey",
                       .shape = control::Shape::Text,
                       .help  = L"an open battle menu's grey: skipped while the slow-down is off; skips, switch-time stops "
                                L"and starts; the filter's effect, count and suppress now",
                       .get   = [] { return control::Value{Reading()}; }});
    if (restores == g_restores.size())
    {
        platform::Log(L"menu grey: installed; an open battle menu's grey follows the slow-down switch");
    }
    else
    {
        platform::LogWarning(L"menu grey: installed, but only {} of {} menu-close restores are followed; a menu closed over a "
                             L"battle menu may bring its grey back with the slow-down off",
                             restores, g_restores.size());
    }
    return true;
}

void Remove() noexcept
{
    for (auto& call : g_restores)
    {
        call.Restore();
    }
    g_open.Restore();
}

void Tick() noexcept
{
    if (!g_open.IsInstalled())
    {
        return;
    }
    Read(reinterpret_cast<const void*>(g_commandGlobal), g_playerOne);  // outside every scope: player 1's own
    if (const bool slows = command_menu_tick::MenuSlow(); slows != g_slows)
    {
        g_slows = slows;
        OnSwitched(slows);
    }
    if (!g_debt || BattleMenuWaits())
    {
        return;
    }
    g_debt = false;
    auto* const filters = static_cast<std::uint8_t*>(Filters());
    FilterState state;
    if (!ReadFilter(filters, state) ||
        !menu_grey_rules::Pays(menu_grey_rules::AtSettle(command_menu_tick::MenuSlow(), Rules(state))))
    {
        return;  // nothing owed (the switch is on again and settled it, or no other owner counts the filter)
    }
    // Another owner still counts the filter and nothing shows it: the game's unsuppress way.
    if (platform::CallGuarded(g_activate, static_cast<void*>(filters), facts::kUiFilterWait, std::uint8_t{0}))
    {
        ++g_settled;
        platform::LogDebug(L"menu grey: no battle menu waits; the grey another owner still counts ({}) is brought back",
                           state.count);
    }
}

}  // namespace lss::battle::menu_grey
