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
#include "players/field_root.h"
#include "players/game_globals.h"
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
using SoundEventFn = void (*)(std::uint8_t id);

// Set at install, then only read.
std::uintptr_t g_filtersGlobal = 0;  // EndUISystem[37]: the UI filter manager
std::uintptr_t g_seadCount = 0, g_seadArray = 0, g_seadMixer = 0;  // the mix's state, for the reading
ActivateFn     g_activate      = nullptr;
DeactivateFn   g_deactivate    = nullptr;
SoundEventFn   g_soundEvent    = nullptr;

platform::CallRedirect                                            g_open;      // the wait's spawn
std::array<platform::CallRedirect, facts::kUiFilterRestoreCalls.size()> g_restores;  // after another menu closes
platform::CallRedirect                                            g_soundOn;   // the wait's muffle, at its open
platform::CallRedirect                                            g_soundOff;  // and at its close

// Game thread.
std::uint32_t g_opensSkipped = 0, g_restoresSkipped = 0, g_flipStops = 0, g_flipStarts = 0, g_settled = 0;
std::uint32_t g_soundOnsSkipped = 0, g_soundOffsSkipped = 0, g_soundOffsHeld = 0, g_soundLifts = 0, g_soundRaises = 0;
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

/// Player 1's command controller. Inside a seat's UI slot scope the slot holds the seat's, so player 1's is then the one
/// noted at the engine tick (a plain game object, no UObject: read guarded, noted again every engine tick).
void* PlayerOneController() noexcept
{
    return players::scopes::OpenPlayer(players::scopes::UiSlot) != 0 ? g_playerOne : players::field_root::CommandController();
}

/// Player 1's or any further player's battle menu waits.
bool BattleMenuWaits() noexcept
{
    if (Waits(PlayerOneController()))
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

/// Every player's wait state by place (0 player 1, then the seats), 0 for a player with no menu ready. An i32, its low byte
/// read.
std::array<std::int32_t, players::kSeatsBuilt + 1> WaitStates() noexcept
{
    std::array<std::int32_t, players::kSeatsBuilt + 1> waits{};
    const auto read = [](const void* controller) -> std::int32_t {
        std::uint8_t state = 0;
        return controller != nullptr && Read(static_cast<const std::uint8_t*>(controller) + facts::kCommandCtrlWaitState, state)
                   ? state
                   : 0;
    };
    waits[0] = read(PlayerOneController());
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        waits[static_cast<std::size_t>(seat)] = command_menu::Ready(seat) ? read(command_menu::Controller(seat)) : 0;
    }
    return waits;
}

/// The wait's muffle raised at its open: the game's call while the slow-down is on, else skipped.
void WaitSoundOn(std::uint8_t id)
{
    if (command_menu_tick::MenuSlow())
    {
        g_soundEvent(id);
        return;
    }
    ++g_soundOnsSkipped;
    platform::LogDebug(L"menu sound: a battle menu opened; the muffle is skipped (the slow-down is switched off)");
}

/// The wait's muffle lifted at its close: skipped with the slow-down off (nothing was raised), held back while another menu
/// still waits, else the game's call.
void WaitSoundOff(std::uint8_t id)
{
    if (!command_menu_tick::MenuSlow())
    {
        ++g_soundOffsSkipped;
        return;
    }
    const auto waits = WaitStates();
    const int  me    = command_menu_tick::Ticking() ? command_menu_tick::TickingSeat() : 0;
    if (menu_grey_rules::HoldSoundOff(me, waits))
    {
        ++g_soundOffsHeld;
        platform::LogDebug(L"menu sound: {}'s battle menu closed; the muffle stays for another player's open menu", players::SeatName(me));
        return;
    }
    g_soundEvent(id);
}

/// The slow-down was switched since the last tick (game thread, outside every scope): the sound follows any waiting menu.
void OnSoundSwitched(bool slows) noexcept
{
    if (!g_soundOn.IsInstalled() || !g_soundOff.IsInstalled())
    {
        return;
    }
    const menu_grey_rules::SoundFlip flip = menu_grey_rules::AtSoundSwitch(slows, WaitStates());
    if (flip == menu_grey_rules::SoundFlip::Nothing)
    {
        return;
    }
    const bool raise = flip == menu_grey_rules::SoundFlip::Raise;
    if (platform::CallGuarded(g_soundEvent, raise ? facts::kSoundEventWaitOn : facts::kSoundEventWaitOff))
    {
        ++(raise ? g_soundRaises : g_soundLifts);
        platform::LogDebug(L"menu sound: the muffle {} for a waiting menu (the slow-down switched {})", raise ? L"raised" : L"lifted",
                           slows ? L"on" : L"off");
    }
    else
    {
        platform::LogWarning(L"menu sound: the game's sound event raised; the muffle may not follow the switch until the menu closes");
    }
}

/// The mix as SEAD last wrote it: world sound's and footsteps' targets and the current effect preset.
std::wstring SoundReading()
{
    std::int32_t count      = 0;
    std::uintptr_t array    = 0;
    std::wstring   text;
    if (Read(reinterpret_cast<const void*>(g_seadCount), count) && Read(reinterpret_cast<const void*>(g_seadArray), array) && array != 0 &&
        count > 0 && count < 1024)
    {
        const auto target = [](std::uintptr_t cat, std::uint32_t member, std::uint32_t offset, float& out) {
            std::uintptr_t faders = 0;
            return Read(reinterpret_cast<const void*>(cat + member), faders) && faders != 0 &&
                   Read(reinterpret_cast<const void*>(faders + offset + facts::kSeadFaderTarget), out);
        };
        const std::uint32_t game = facts::kSeadVolumeLayerGame * facts::kSeadFaderBytes;
        for (std::int32_t i = 0; i < count; ++i)
        {
            std::uintptr_t cat = 0;
            std::int32_t   id  = -1;
            if (!Read(reinterpret_cast<const void*>(array + static_cast<std::uintptr_t>(i) * sizeof(void*)), cat) || cat == 0 ||
                !Read(reinterpret_cast<const void*>(cat + facts::kSeadCategoryId), id))
            {
                continue;
            }
            float volume = -1.0f, pitch = -1.0f;
            if (id == facts::kSeadCategorySe3d && target(cat, facts::kSeadCategoryVolumes, game, volume) &&
                target(cat, facts::kSeadCategoryPitch, 0, pitch))
            {
                text += std::format(L"world sound volume {:.3f} pitch {:.3f}; ", volume, pitch);
            }
            else if (id == facts::kSeadCategoryMasts && target(cat, facts::kSeadCategoryVolumes, game, volume))
            {
                text += std::format(L"footsteps volume {:.3f}; ", volume);
            }
        }
    }
    std::uintptr_t mixer  = 0;
    std::int32_t   preset = -1;
    const bool     read   = Read(reinterpret_cast<const void*>(g_seadMixer), mixer) && mixer != 0 &&
                      Read(reinterpret_cast<const void*>(mixer + facts::kSeadMixerPreset), preset);
    text += read ? std::format(L"effect preset {}{}", preset, preset == facts::kSeadPresetWaitMode ? L" (the muffle's)" : L"")
                 : std::wstring(L"effect preset unread");
    const auto waits = WaitStates();
    return std::format(L"{}; the wait's muffle {}; {}; waits: player 1 {}, player 2 {}, player 3 {}; raises skipped {}, lifts skipped {}, lifts "
                       L"held for another menu {}, lifted at a switch {}, raised at a switch {}",
                       g_soundOn.IsInstalled() && g_soundOff.IsInstalled() ? L"on" : L"off",
                       command_menu_tick::MenuSlow() ? L"follows the game" : L"skipped", text, waits[0], waits[1], waits[2], g_soundOnsSkipped,
                       g_soundOffsSkipped, g_soundOffsHeld, g_soundLifts, g_soundRaises);
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
    const auto filters    = players::game_globals::UiSlotAddress(facts::kUiFilterSlot);
    const auto activate   = platform::FindFunction(image, facts::kUiFilterActivate);
    const auto deactivate = platform::FindFunction(image, facts::kUiFilterDeactivate);
    const auto openSite   = image.Rebase(facts::kUiFilterWaitOpenCall, 5);
    if (!filters || !players::field_root::Installed() || !activate || !deactivate || !openSite)
    {
        platform::Log(L"menu grey: an engine check failed; an open battle menu greys the screen and muffles the sound even with the "
                      L"slow-down off");
        return false;
    }
    g_filtersGlobal = filters;
    g_activate      = reinterpret_cast<ActivateFn>(*activate);
    g_deactivate    = reinterpret_cast<DeactivateFn>(*deactivate);
    if (!g_open.Install(*openSite, *activate, reinterpret_cast<std::uintptr_t>(&Open)))
    {
        platform::Log(L"menu grey: the wait's grey call could not be redirected; an open battle menu greys the screen and muffles the "
                      L"sound even with the slow-down off");
        return false;
    }
    std::uint32_t restores = 0;
    for (std::size_t i = 0; i < g_restores.size(); ++i)
    {
        const auto site = image.Rebase(facts::kUiFilterRestoreCalls[i], 5);
        restores += site && g_restores[i].Install(*site, *activate, reinterpret_cast<std::uintptr_t>(&RestoreAfterMenu)) ? 1 : 0;
    }
    // The muffle: both calls or neither, so a raise never goes without its lift. After the grey: the open call shares its block.
    const auto soundEvent = platform::FindFunction(image, facts::kSoundEvent);
    const auto soundOn    = image.Rebase(facts::kWaitSoundOnCall, 5);
    const auto soundOff   = image.Rebase(facts::kWaitSoundOffCall, 5);
    const auto seadCount  = image.Rebase(facts::kSeadCategoryCount, sizeof(std::int32_t));
    const auto seadArray  = image.Rebase(facts::kSeadCategories, sizeof(void*));
    const auto seadMixer  = image.Rebase(facts::kSeadMixer, sizeof(void*));
    if (soundEvent && soundOn && soundOff && seadCount && seadArray && seadMixer)
    {
        g_soundEvent = reinterpret_cast<SoundEventFn>(*soundEvent);
        g_seadCount  = *seadCount;
        g_seadArray  = *seadArray;
        g_seadMixer  = *seadMixer;
        if (!g_soundOn.Install(*soundOn, *soundEvent, reinterpret_cast<std::uintptr_t>(&WaitSoundOn)) ||
            !g_soundOff.Install(*soundOff, *soundEvent, reinterpret_cast<std::uintptr_t>(&WaitSoundOff)))
        {
            g_soundOff.Restore();
            g_soundOn.Restore();
        }
    }
    const bool sound = g_soundOn.IsInstalled() && g_soundOff.IsInstalled();
    if (!sound)
    {
        platform::LogWarning(L"menu sound: the wait's sound calls are not what the research read; an open battle menu muffles the sound "
                             L"even with the slow-down off");
    }
    g_slows = command_menu_tick::MenuSlow();
    if (sound)
    {
        platform::Log(L"menu sound: installed; an open battle menu's muffle follows the slow-down switch");
        control::Register({.name  = L"menu-sound",
                           .shape = control::Shape::Text,
                           .help  = L"an open battle menu's muffle: the mix's world sound and footsteps, its effect preset, every player's "
                                    L"wait state, and the raises and lifts skipped, held and made at a switch",
                           .get   = [] {
                               try
                               {
                                   return control::Value{SoundReading()};
                               }
                               catch (...)
                               {
                                   return control::Value{std::wstring(L"unreadable (an exception)")};
                               }
                           }});
    }
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
    // The muffle's first: the open call's block holds the grey's redirected call in its snapshot.
    g_soundOff.Restore();
    g_soundOn.Restore();
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
    g_playerOne = players::field_root::CommandController();  // outside every scope: player 1's own
    if (const bool slows = command_menu_tick::MenuSlow(); slows != g_slows)
    {
        g_slows = slows;
        OnSwitched(slows);
        OnSoundSwitched(slows);
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
