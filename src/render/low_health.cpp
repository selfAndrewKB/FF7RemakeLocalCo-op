#include "render/low_health.h"

#include "battle/command_menu_tick.h"
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
#include "render/low_health_rules.h"
#include "render/view_places.h"
#include "views/columns.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::render::low_health {

namespace {

namespace f     = facts;
namespace rules = low_health_rules;
using platform::Read;

using HpFn        = std::uint32_t (*)(std::uint8_t type);
using FilterTickFn = void (*)(void* holder, float dt);

// Set at install, then only read.
HpFn         g_hp         = nullptr;
FilterTickFn g_filterTick = nullptr;
float        g_on         = 0.0f;  // the game's intensity with the effect up
float        g_fadeLength = 0.0f;  // the game's fade length, seconds

platform::CallRedirect g_hpCall, g_tickCall;

// Game thread.
bool                 g_perView = true;  // the switch
views::columns::Map  g_map;             // the engine's list as columns, this tick
bool                 g_severalViews = false;

/// A player's own fade: a holder of the game's layout, run by the game's tick (its component pointer stays null).
struct alignas(16) Holder
{
    std::array<std::byte, f::kScreenFilterHolderBytes> bytes{};

    float Get(std::uint32_t offset) const noexcept
    {
        float value = 0.0f;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
    void Set(std::uint32_t offset, float value) noexcept { std::memcpy(bytes.data() + offset, &value, sizeof(value)); }
};
std::array<Holder, rules::kPlayers> g_fades{};
std::array<bool, rules::kPlayers>   g_differs{};  // the player's view took a value below the game's at its last blend

/// What Before changed on the component, put back by After.
struct Lent
{
    std::uint8_t* component = nullptr;
    std::uint32_t flags     = 0;
    float         intensity = 0.0f;
};
Lent g_lent;

std::uint32_t g_ticksSeen = 0, g_viewsWritten = 0, g_viewsUnknown = 0;
bool          g_scopeTold = false;

/// The command controller of `player`: player 1's from the UI slot (outside every scope), a seat's as bound.
const std::uint8_t* ControllerOf(int player) noexcept
{
    return player == 0 ? players::field_root::CommandController()
                       : static_cast<const std::uint8_t*>(players::scopes::CommandController(player));
}

/// A controller's byte at `offset`, or `fallback` for no controller or a faulting read.
std::uint8_t ByteOf(const std::uint8_t* controller, std::uint32_t offset, std::uint8_t fallback) noexcept
{
    std::uint8_t value = fallback;
    return controller != nullptr && Read(controller + offset, value) ? value : fallback;
}

/// The danger check's HP read: the ticking controller's own members only (rules::Counts), the game's read for those.
std::uint32_t HpForTickingController(std::uint8_t type)
{
    if (!g_perView || !battle::command_menu_tick::Ticking())
    {
        return g_hp(type);
    }
    rules::Held held{};
    held[0] = rules::kNoType;
    for (int seat = 1; seat < rules::kPlayers; ++seat)
    {
        held[static_cast<std::size_t>(seat)] = players::scopes::MemberType(seat);
    }
    return rules::Counts(battle::command_menu_tick::TickingSeat(), type, held) ? g_hp(type) : f::kDyingNeverLow;
}

/// Sets player `player`'s fade going toward `target` the way the game's setter sets its own: from the current value,
/// elapsed zero, over the game's length.
void Arm(Holder& fade, float target) noexcept
{
    fade.Set(f::kDyingFadeStart, fade.Get(f::kDyingFadeNow));
    fade.Set(f::kDyingFadeTarget, target);
    fade.Set(f::kDyingFadeElapsed, 0.0f);
    fade.Set(f::kDyingFadeLength, g_fadeLength);
}

/// Each player's fade follows their controller's flag (player 1's allowed byte gates everyone's).
void ArmFades() noexcept
{
    if (players::scopes::OpenPlayer(players::scopes::UiSlot) != 0)
    {
        if (!g_scopeTold)
        {
            g_scopeTold = true;
            platform::LogWarning(L"low health: the HUD tick ran inside a player's scope; the views' fades wait for the next one");
        }
        return;
    }
    const std::uint8_t allowed = ByteOf(ControllerOf(0), f::kCommandCtrlDyingAllowed, 1);
    for (int player = 0; player < rules::kPlayers; ++player)
    {
        Holder&     fade   = g_fades[static_cast<std::size_t>(player)];
        const bool  danger = ByteOf(ControllerOf(player), f::kCommandCtrlDying, 0) != 0;
        const float want   = rules::Target(danger, allowed != 0, g_on);
        if (fade.Get(f::kDyingFadeTarget) == want)
        {
            continue;
        }
        Arm(fade, want);
        const std::wstring who = player == 0 ? std::wstring(L"player 1's character or a member no player controls")
                                             : std::format(L"{}'s character", players::SeatName(player));
        if (want > 0.0f)
        {
            platform::Log(L"low health: {} in danger: their view fades the red edge in", who);
        }
        else
        {
            platform::Log(L"low health: {} out of danger{}: the red edge fades out of their view", who,
                          danger ? L" (an event hides the effect)" : L"");
        }
    }
}

/// The game's fade tick on the screen-filter slot, then each player's fade on the same frame time.
void FilterTick(void* holder, float dt)
{
    g_filterTick(holder, dt);
    ++g_ticksSeen;
    ArmFades();
    for (Holder& fade : g_fades)
    {
        g_filterTick(fade.bytes.data(), dt);
    }
}

std::uint8_t* Component() noexcept
{
    void* const holder = players::game_globals::UiSlot(f::kScreenFilterSlot);
    return holder != nullptr ? static_cast<std::uint8_t*>(platform::ReadPointer(holder, f::kScreenFilterComponent)) : nullptr;
}

/// Debug once per change of whether a player's view takes less than the game's value.
void Note(int player, bool differs, float own, float game)
{
    bool& was = g_differs[static_cast<std::size_t>(player)];
    if (was == differs)
    {
        return;
    }
    was = differs;
    platform::LogDebug(L"low health: {}'s view takes {} red edge ({:.2f}; the game's {:.2f})", players::SeatName(player),
                       differs ? L"its own" : L"the game's", own, game);
}

std::wstring Reading()
{
    std::uint8_t* const component = Component();
    std::uint32_t       flags     = 0;
    float               game      = 0.0f;
    const bool          read      = component != nullptr && Read(component + f::kImageEffectsSettingsFlags, flags) &&
                      Read(component + f::kImageEffectsNarrowNow, game);
    std::wstring text = std::format(L"{}; {}; the game's effect {}", g_perView ? L"each view its own" : L"the game's (off)",
                                    g_severalViews ? L"several views" : L"one view",
                                    read ? std::format(L"{:.2f}{}", game, (flags & f::kImageEffectsNarrowBit) != 0 ? L"" : L" (bit off)")
                                         : std::wstring(L"unread"));
    for (int player = 0; player < rules::kPlayers; ++player)
    {
        const Holder& fade = g_fades[static_cast<std::size_t>(player)];
        text += std::format(L"; {}: danger {}, fade {:.2f} toward {:.2f}", players::SeatName(player),
                            ByteOf(ControllerOf(player), f::kCommandCtrlDying, 0), fade.Get(f::kDyingFadeNow),
                            fade.Get(f::kDyingFadeTarget));
    }
    return text + std::format(L"; HUD ticks {}, views given their own {}, views no player's {}", g_ticksSeen, g_viewsWritten,
                              g_viewsUnknown);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto hp        = platform::FindFunction(image, f::kPartyHpByType);
    const auto tick      = platform::FindFunction(image, f::kScreenFilterTick);
    const auto hpSite    = image.Rebase(f::kDyingHpCall, 5);
    const auto tickSite  = image.Rebase(f::kScreenFilterTickCall, 5);
    const auto on        = image.Rebase(f::kDyingIntensityOn, sizeof(float));
    const auto length    = image.Rebase(f::kDyingFadeSeconds, sizeof(float));
    float      onValue = 0.0f, lengthValue = 0.0f;
    if (!hp || !tick || !hpSite || !tickSite || !on || !length || !players::field_root::Installed() ||
        !Read(reinterpret_cast<const void*>(*on), onValue) || !Read(reinterpret_cast<const void*>(*length), lengthValue) ||
        !(onValue > 0.0f && onValue <= 1.0f) || !(lengthValue > 0.0f && lengthValue < 5.0f))
    {
        platform::Log(L"low health: an engine check failed; one player's low health lights the red edge in every view");
        return false;
    }
    g_hp         = reinterpret_cast<HpFn>(*hp);
    g_filterTick = reinterpret_cast<FilterTickFn>(*tick);
    g_on         = onValue;
    g_fadeLength = lengthValue;
    // The fades first: a decision per player with no fade per view would leave a player's view without the edge.
    if (!g_tickCall.Install(*tickSite, *tick, reinterpret_cast<std::uintptr_t>(&FilterTick)) ||
        !g_hpCall.Install(*hpSite, *hp, reinterpret_cast<std::uintptr_t>(&HpForTickingController)))
    {
        g_hpCall.Restore();
        g_tickCall.Restore();
        platform::Log(L"low health: a call is not what the research read; one player's low health lights the red edge in every "
                      L"view");
        return false;
    }
    control::Register({.name  = L"low-health-per-view",
                       .shape = control::Shape::OnOff,
                       .help  = L"the low-health red edge in each player's own view, fading on that player's own danger (on); "
                                L"off: the game's whole-party edge in every view",
                       .set   = [](const control::Value& value) {
                           g_perView = std::get<bool>(value);
                           platform::Log(L"low health: {}", g_perView ? L"each view takes its own player's red edge"
                                                                     : L"every view takes the game's whole-party red edge");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_perView}; }});
    control::Register({.name  = L"render.low-health",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the game's red edge, each player's danger flag and own fade, and the views given "
                                L"their own since the start",
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
    platform::Log(L"low health: installed; each player's view fades the red edge on that player's own danger (an AI member's on "
                  L"player 1's)");
    return true;
}

void Remove() noexcept
{
    g_hpCall.Restore();
    g_tickCall.Restore();
}

void Tick(const views::columns::Map& map) noexcept
{
    g_map          = map;
    g_severalViews = map.count > 1;
}

void Before(const void* view) noexcept
{
    g_lent = {};
    if (!g_perView || !g_severalViews || !g_tickCall.IsInstalled())
    {
        return;
    }
    std::uint8_t* const component = Component();
    std::uint32_t       flags     = 0;
    float               game      = 0.0f;
    if (component == nullptr || !Read(component + f::kImageEffectsSettingsFlags, flags) ||
        (flags & f::kImageEffectsNarrowBit) == 0 || !Read(component + f::kImageEffectsNarrowNow, game))
    {
        return;  // no effect up: every view takes nothing
    }
    const int player = rules::ViewPlayer(view_places::PlaceOf(view), g_map);
    if (player < 0)
    {
        ++g_viewsUnknown;
        return;  // a capture or reflection, or the list not yet published: the game's value
    }
    const float own  = g_fades[static_cast<std::size_t>(player)].Get(f::kDyingFadeNow);
    const float take = rules::ViewIntensity(own, game);
    Note(player, take < game, own, game);
    if (take >= game)
    {
        return;
    }
    const std::uint32_t lentFlags = take > 0.0f ? flags : flags & ~f::kImageEffectsNarrowBit;
    if (!platform::WriteGuarded(component + f::kImageEffectsNarrowNow, &take, sizeof(take)))
    {
        return;
    }
    g_lent = {.component = component, .flags = flags, .intensity = game};
    if (lentFlags != flags)
    {
        platform::WriteGuarded(component + f::kImageEffectsSettingsFlags, &lentFlags, sizeof(lentFlags));
    }
    ++g_viewsWritten;
}

void After() noexcept
{
    if (g_lent.component == nullptr)
    {
        return;
    }
    platform::WriteGuarded(g_lent.component + f::kImageEffectsNarrowNow, &g_lent.intensity, sizeof(g_lent.intensity));
    platform::WriteGuarded(g_lent.component + f::kImageEffectsSettingsFlags, &g_lent.flags, sizeof(g_lent.flags));
    g_lent = {};
}

}  // namespace lss::render::low_health
