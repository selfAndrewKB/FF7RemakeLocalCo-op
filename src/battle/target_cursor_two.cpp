#include "battle/target_cursor_two.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "battle/targeting_two.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "platform/byte_scope.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"
#include "players/unreal_call.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>

namespace lss::battle::target_cursor_two {

namespace {

using platform::Read;
using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

using SysFn      = void (*)(void* system);
using SetColorFn = void (*)(void* widget, const float* color);

constexpr int kPlayer = 1;

// Set once at install.
void**         g_system   = nullptr;
std::uint8_t** g_barSlot  = nullptr;  // EndUISystem[27]
std::uint8_t** g_iconSlot = nullptr;  // EndUISystem[38]
SysFn          g_driver   = nullptr;
std::uintptr_t g_lineOfSight = 0, g_lineOfSightCall = 0, g_suspend = 0;
SetColorFn     g_setColor = nullptr;
platform::CallRedirect  g_lineOfSightRedirect;  // the call crosses a block: placed from the game thread
platform::EntryRedirect g_suspendRedirect;

// Game thread.
platform::ByteContext g_copy;  // player 2's cursor slot and HUD keys
void*                 g_copySystem = nullptr;
std::uint8_t*         g_copyBar = nullptr, *g_copyIcon = nullptr;
bool                  g_placed = false;
UFunction*            g_hide   = nullptr;  // /Script/EndGame.EndMenuAPI:HideBattleTargetIcon
UObject*              g_api    = nullptr;  // its class default object
bool                  g_hideLookedUp = false;
int                   g_column = hud::overlay_pass::kSingle;  // the column whose pass runs (hud/overlay_pass.h)
void*                 g_hidden = nullptr;  // the widget made transparent for this pass
alignas(16) float     g_hiddenColor[4]{};

struct Counts
{
    std::uint32_t runs = 0, shown = 0, releases = 0, lineOfSight = 0, hides = 0;
};
Counts g_counts;
bool   g_cursorSeen = false;  // the copy held a widget at the last run

void* System() noexcept
{
    void* system = nullptr;
    return Read(g_system, system) ? system : nullptr;
}

void* CopyWidget() noexcept
{
    void* widget = nullptr;
    if (g_copy.Defined())
    {
        std::memcpy(&widget, g_copy.Value(0).data(), sizeof(widget));
    }
    return widget;
}

/// Player 2's copy for this battle system and these UI objects (all zero: no cursor, no keys).
bool CopyFor(void* system) noexcept
{
    std::uint8_t* bar  = nullptr;
    std::uint8_t* icon = nullptr;
    if (!Read(g_barSlot, bar) || !Read(g_iconSlot, icon) || bar == nullptr || icon == nullptr)
    {
        return false;
    }
    if (g_copy.Defined() && system == g_copySystem && bar == g_copyBar && icon == g_copyIcon)
    {
        return true;
    }
    if (CopyWidget() != nullptr)
    {
        End();  // the old copy's cursor released before the copy goes
    }
    const platform::ByteContext::Range ranges[] = {
        {reinterpret_cast<std::uintptr_t>(system) + facts::kBattleSysCursorWidget, sizeof(void*), platform::ByteContext::Kind::Exchange},
        {reinterpret_cast<std::uintptr_t>(bar) + facts::kEnemyBarDisplayedTarget, facts::kEnemyBarDisplayedTargetBytes, platform::ByteContext::Kind::Exchange},
        {reinterpret_cast<std::uintptr_t>(icon) + facts::kOutsideIconTargetName, facts::kOutsideIconTargetNameBytes, platform::ByteContext::Kind::Exchange},
    };
    if (!g_copy.Define(ranges))
    {
        return false;
    }
    g_copySystem = system;
    g_copyBar    = bar;
    g_copyIcon   = icon;
    platform::LogDebug(L"target cursor 2: player 2's cursor slot and HUD keys start empty on battle system {:#x}", reinterpret_cast<std::uintptr_t>(system));
    return true;
}

bool LookUpHide() noexcept
try
{
    if (g_hideLookedUp)
    {
        return g_hide != nullptr;
    }
    g_hideLookedUp = true;
    g_hide         = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/EndGame.EndMenuAPI:HideBattleTargetIcon"));
    auto* type     = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/EndGame.EndMenuAPI"));
    g_api          = type == nullptr ? nullptr : static_cast<UObject*>(type->GetClassDefaultObject());
    if (g_hide == nullptr || g_api == nullptr || g_hide->GetParmsSize() != sizeof(void*))
    {
        g_hide = nullptr;
        platform::Log(L"target cursor 2: EndMenuAPI.HideBattleTargetIcon is not the function the mod expects; player 2's cursor is not released on a suspension");
        return false;
    }
    return true;
}
catch (...)
{
    g_hide = nullptr;
    return false;
}

/// The game's own release of a cursor widget (the three steps its inline releases make), on player 2's.
void ReleaseCopy(const wchar_t* why) noexcept
{
    void* widget = CopyWidget();
    if (widget == nullptr)
    {
        return;
    }
    bool hidden = false;
    if (LookUpHide())
    {
        void* frame = widget;
        hidden      = players::GuardedProcessEvent(g_api, g_hide, &frame);
    }
    void* none = nullptr;
    std::memcpy(g_copy.Value(0).data(), &none, sizeof(none));
    g_cursorSeen = false;
    ++g_counts.releases;
    platform::LogDebug(L"target cursor 2: player 2's cursor {} ({})", hidden ? L"released with the game's hide" : L"DROPPED (the hide could not run)", why);
}

/// The line-of-sight step at the game's call: the game's run, then player 2's with its cursor and trace.
void LineOfSight(void* system)
{
    reinterpret_cast<SysFn>(g_lineOfSight)(system);
    if (players::scopes::Member(kPlayer) == nullptr || !g_copy.Defined() || system != g_copySystem || CopyWidget() == nullptr)
    {
        return;
    }
    const players::scopes::Scope     scope(kPlayer, players::scopes::Primary | players::scopes::Leader);
    const targeting_two::FieldsScope fields;
    const platform::ByteScope        copy(g_copy);
    if (scope.Held() && fields.Held() && copy.Held())
    {
        ++g_counts.lineOfSight;
        platform::CallGuarded(reinterpret_cast<SysFn>(g_lineOfSight), system);
    }
}

/// The battle suspension: the game releases player 1's cursor; player 2's goes the same way.
void Suspend(void* system)
{
    reinterpret_cast<SysFn>(g_suspendRedirect.Original())(system);
    if (g_copy.Defined() && system == g_copySystem)
    {
        ReleaseCopy(L"the battle was suspended");
    }
}

void Transparent(void* widget) noexcept
{
    alignas(16) float colour[4];
    if (widget == nullptr || !Read(static_cast<const std::uint8_t*>(widget) + facts::kUserWidgetColor, colour) || colour[3] == 0.0f)
    {
        return;
    }
    std::memcpy(g_hiddenColor, colour, sizeof(colour));
    colour[3] = 0.0f;
    if (platform::CallGuarded(g_setColor, widget, static_cast<const float*>(colour)))
    {
        g_hidden = widget;
        ++g_counts.hides;
    }
}

template <class Fn>
bool FindInto(const platform::GameImage& image, const platform::FunctionFact& fact, Fn& out) noexcept
{
    const auto address = platform::FindFunction(image, fact);
    out                = address ? reinterpret_cast<Fn>(*address) : nullptr;
    return out != nullptr;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system   = image.Rebase(facts::kBattleSystemGlobal, sizeof(void*));
    const auto bar      = image.Rebase(facts::kEnemyBarSlotGlobal, sizeof(void*));
    const auto icon     = image.Rebase(facts::kOutsideIconSlotGlobal, sizeof(void*));
    const auto los      = platform::FindFunction(image, facts::kCursorLineOfSight);
    const auto losCall  = image.Rebase(facts::kCursorLineOfSightCall, 5);
    const auto suspend  = platform::FindFunction(image, facts::kBattleSuspend);
    const auto setColor = image.Rebase(facts::kSetColorAndOpacity);
    if (!system || !bar || !icon || !los || !losCall || !suspend || !setColor || !FindInto(image, facts::kCursorDriver, g_driver))
    {
        platform::Log(L"target cursor 2: an engine check failed; player 2 has no cursor of its own");
        return false;
    }
    g_system          = reinterpret_cast<void**>(*system);
    g_barSlot         = reinterpret_cast<std::uint8_t**>(*bar);
    g_iconSlot        = reinterpret_cast<std::uint8_t**>(*icon);
    g_lineOfSight     = *los;
    g_lineOfSightCall = *losCall;
    g_suspend         = *suspend;
    g_setColor        = reinterpret_cast<SetColorFn>(*setColor);
    control::Register({.name = L"target-cursor-two", .shape = control::Shape::Text,
                       .help = L"player 2's cursor: driver runs, runs with a cursor, releases, line-of-sight runs, per-pass hides",
                       .get  = [] {
                           return control::Value{std::format(L"runs {} (with a cursor {}), releases {}, line of sight {}, hides {}; cursor now {}",
                                                             g_counts.runs, g_counts.shown, g_counts.releases, g_counts.lineOfSight,
                                                             g_counts.hides, CopyWidget() != nullptr ? L"shown" : L"none")};
                       }});
    return true;
}

void Remove() noexcept
{
    g_suspendRedirect.Restore();
    g_lineOfSightRedirect.Restore();
}

void Tick() noexcept
{
    if (g_placed || g_driver == nullptr)
    {
        return;
    }
    g_placed = true;  // once, whatever the outcome
    const bool los = g_lineOfSightRedirect.InstallFromOwnThread(g_lineOfSightCall, g_lineOfSight, reinterpret_cast<std::uintptr_t>(&LineOfSight));
    // The suspension's entry opens with three position-independent instructions (5 bytes); the game thread alone
    // runs it, and it is here, outside [ASSUMPTION: its delegate tables fire on the game thread].
    const bool suspend = g_suspendRedirect.Install(g_suspend, std::span(facts::kBattleSuspendEntry.data(), facts::kBattleSuspendFirstBytes),
                                                   reinterpret_cast<std::uintptr_t>(&Suspend));
    platform::Log(L"target cursor 2: installed; the game's cursor driver runs again for player 2 (line of sight {}, suspension {})",
                  los ? L"redirected" : L"NOT redirected", suspend ? L"redirected" : L"NOT redirected");
}

void RunForPlayerTwo(void* system) noexcept
{
    if (g_driver == nullptr || !CopyFor(system))
    {
        return;
    }
    const platform::ByteScope copy(g_copy);
    if (!copy.Held())
    {
        return;
    }
    ++g_counts.runs;
    if (!platform::CallGuarded(g_driver, system))
    {
        platform::Log(L"target cursor 2: the cursor driver for player 2 RAISED AN EXCEPTION");
    }
    const bool shown = CopyWidget() != nullptr;
    if (shown)
    {
        ++g_counts.shown;
    }
    if (shown != g_cursorSeen)
    {
        g_cursorSeen = shown;
        platform::LogDebug(L"target cursor 2: player 2's cursor {} (the game's driver, player 2's target)", shown ? L"shown" : L"gone");
    }
}

void End() noexcept
{
    if (!g_copy.Defined())
    {
        return;
    }
    ReleaseCopy(L"the battle ended or player 2 left");
    g_copy.Clear();
    g_copySystem = nullptr;
}

void BeginPass(int column) noexcept
{
    g_column = column;
    g_hidden = nullptr;
    if (column < 0 || !g_copy.Defined())
    {
        return;
    }
    void* system = System();
    if (system == nullptr || system != g_copySystem)
    {
        return;
    }
    if (column >= 1)  // every further column shows player 2's cursor until the module keeps one per seat (seats.md step 3)
    {
        void* one = nullptr;
        if (Read(static_cast<const std::uint8_t*>(system) + facts::kBattleSysCursorWidget, one))
        {
            Transparent(one);  // player 1's cursor out of the further column
        }
    }
    else
    {
        Transparent(CopyWidget());  // player 2's cursor out of player 1's column
    }
}

void EndPass() noexcept
{
    if (g_hidden != nullptr)
    {
        platform::CallGuarded(g_setColor, g_hidden, static_cast<const float*>(g_hiddenColor));
        g_hidden = nullptr;
    }
    g_column = hud::overlay_pass::kSingle;
}

}  // namespace lss::battle::target_cursor_two
