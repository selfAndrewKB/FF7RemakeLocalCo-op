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
#include "hud/overlay_pass.h"
#include "platform/byte_scope.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/unreal_call.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>

namespace lss::battle::target_cursor_two {

namespace {

using platform::Read;
using players::kSeatsBuilt;
using players::SeatBuilt;
using players::SeatName;
using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

using SysFn      = void (*)(void* system);
using SetColorFn = void (*)(void* widget, const float* color);

// Set once at install.
void**         g_system   = nullptr;
std::uint8_t** g_barSlot  = nullptr;  // EndUISystem[27]
std::uint8_t** g_iconSlot = nullptr;  // EndUISystem[38]
SysFn          g_driver   = nullptr;
std::uintptr_t g_lineOfSight = 0, g_lineOfSightCall = 0, g_suspend = 0;
SetColorFn     g_setColor = nullptr;
platform::CallRedirect  g_lineOfSightRedirect;  // the call crosses a block: placed from the game thread
platform::EntryRedirect g_suspendRedirect;

struct Counts
{
    std::uint32_t runs = 0, shown = 0, releases = 0, lineOfSight = 0;
};

// Game thread. Index = seat ([0] unused): each seat's cursor slot and HUD keys, copied.
struct Seat
{
    platform::ByteContext copy;              // the seat's cursor slot and HUD keys
    void*                 system     = nullptr;
    std::uint8_t*         bar        = nullptr;
    std::uint8_t*         icon       = nullptr;
    Counts                counts;
    bool                  cursorSeen = false;  // the copy held a widget at the last run
};
std::array<Seat, players::scopes::kMaxPlayers> g_seats;
bool                                           g_placed = false;
UFunction*                                     g_hide   = nullptr;  // /Script/EndGame.EndMenuAPI:HideBattleTargetIcon
UObject*                                       g_api    = nullptr;  // its class default object
bool                                           g_hideLookedUp = false;
std::uint32_t                                  g_hides  = 0;  // per-pass hides, all passes

/// The widgets made transparent for this pass (player 1's and the other seats'), put back at its end.
struct Hidden
{
    void*             widget = nullptr;
    alignas(16) float color[4]{};
};
std::array<Hidden, players::scopes::kMaxPlayers> g_hidden;
int                                              g_hiddenCount = 0;

void* System() noexcept
{
    void* system = nullptr;
    return Read(g_system, system) ? system : nullptr;
}

void* CopyWidget(const Seat& s) noexcept
{
    void* widget = nullptr;
    if (s.copy.Defined())
    {
        std::memcpy(&widget, s.copy.Value(0).data(), sizeof(widget));
    }
    return widget;
}

void ReleaseCopy(int seat, const wchar_t* why) noexcept;

/// The seat's copy for this battle system and these UI objects (all zero: no cursor, no keys).
bool CopyFor(int seat, void* system) noexcept
{
    Seat&         s    = g_seats[seat];
    std::uint8_t* bar  = nullptr;
    std::uint8_t* icon = nullptr;
    if (!Read(g_barSlot, bar) || !Read(g_iconSlot, icon) || bar == nullptr || icon == nullptr)
    {
        return false;
    }
    if (s.copy.Defined() && system == s.system && bar == s.bar && icon == s.icon)
    {
        return true;
    }
    if (CopyWidget(s) != nullptr)
    {
        ReleaseCopy(seat, L"the battle system or the HUD objects changed");  // the old copy's cursor released before the copy goes
    }
    const platform::ByteContext::Range ranges[] = {
        {reinterpret_cast<std::uintptr_t>(system) + facts::kBattleSysCursorWidget, sizeof(void*), platform::ByteContext::Kind::Exchange},
        {reinterpret_cast<std::uintptr_t>(bar) + facts::kEnemyBarDisplayedTarget, facts::kEnemyBarDisplayedTargetBytes, platform::ByteContext::Kind::Exchange},
        {reinterpret_cast<std::uintptr_t>(icon) + facts::kOutsideIconTargetName, facts::kOutsideIconTargetNameBytes, platform::ByteContext::Kind::Exchange},
    };
    if (!s.copy.Define(ranges))
    {
        s.system = nullptr;  // the record empty, as after End
        s.bar = s.icon = nullptr;
        return false;
    }
    s.system = system;
    s.bar    = bar;
    s.icon   = icon;
    platform::LogDebug(L"target cursor 2: {}'s cursor slot and HUD keys start empty on battle system {:#x}", SeatName(seat),
                       reinterpret_cast<std::uintptr_t>(system));
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
        platform::Log(L"target cursor 2: EndMenuAPI.HideBattleTargetIcon is not the function the mod expects; every seat's cursor is dropped without the game's hide when it goes");
        return false;
    }
    return true;
}
catch (...)
{
    g_hide = nullptr;
    return false;
}

/// The game's own release of a cursor widget (the three steps its inline releases make), on the seat's.
void ReleaseCopy(int seat, const wchar_t* why) noexcept
{
    Seat& s      = g_seats[seat];
    void* widget = CopyWidget(s);
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
    std::memcpy(s.copy.Value(0).data(), &none, sizeof(none));
    s.cursorSeen = false;
    ++s.counts.releases;
    platform::LogDebug(L"target cursor 2: {}'s cursor {} ({})", SeatName(seat),
                       hidden ? L"released with the game's hide" : L"DROPPED (the hide could not run)", why);
}

/// The line-of-sight step at the game's call: the game's run, then each seat's with its cursor and trace.
void LineOfSight(void* system)
{
    reinterpret_cast<SysFn>(g_lineOfSight)(system);
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        Seat& s = g_seats[seat];
        if (players::scopes::Member(seat) == nullptr || !s.copy.Defined() || system != s.system || CopyWidget(s) == nullptr)
        {
            continue;
        }
        const players::scopes::Scope     scope(seat, players::scopes::Primary | players::scopes::Leader);
        const targeting_two::FieldsScope fields(seat);
        const platform::ByteScope        copy(s.copy);
        if (scope.Held() && fields.Held() && copy.Held())
        {
            ++s.counts.lineOfSight;
            platform::CallGuarded(reinterpret_cast<SysFn>(g_lineOfSight), system);
        }
    }
}

/// The battle suspension: the game releases player 1's cursor; every seat's goes the same way.
void Suspend(void* system)
{
    reinterpret_cast<SysFn>(g_suspendRedirect.Original())(system);
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (g_seats[seat].copy.Defined() && system == g_seats[seat].system)
        {
            ReleaseCopy(seat, L"the battle was suspended");
        }
    }
}

void Transparent(void* widget) noexcept
{
    alignas(16) float colour[4];
    if (widget == nullptr || g_hiddenCount >= static_cast<int>(g_hidden.size()) ||
        !Read(static_cast<const std::uint8_t*>(widget) + facts::kUserWidgetColor, colour) || colour[3] == 0.0f)
    {
        return;
    }
    Hidden& hidden = g_hidden[g_hiddenCount];
    std::memcpy(hidden.color, colour, sizeof(colour));
    colour[3] = 0.0f;
    if (platform::CallGuarded(g_setColor, widget, static_cast<const float*>(colour)))
    {
        hidden.widget = widget;
        ++g_hiddenCount;
        ++g_hides;
    }
}

/// Each seat's driver runs, runs with a cursor, releases and line-of-sight runs, its cursor now; the per-pass hides.
std::wstring Reading()
{
    std::wstring text;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        text += std::format(L"{}{}: runs {} (with a cursor {}), releases {}, line of sight {}, cursor now {}", seat == 1 ? L"" : L"; ",
                            SeatName(seat), s.counts.runs, s.counts.shown, s.counts.releases, s.counts.lineOfSight,
                            CopyWidget(s) != nullptr ? L"shown" : L"none");
    }
    return text + std::format(L"; hides {}", g_hides);
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
        platform::Log(L"target cursor 2: an engine check failed; no seat has a cursor of its own");
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
                       .help = L"each seat's cursor: driver runs, runs with a cursor, releases, line-of-sight runs, its cursor now; "
                               L"the per-pass hides",
                       .get  = [] { return control::Value{Reading()}; }});
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
    platform::Log(L"target cursor 2: installed; the game's cursor driver runs again for each seat (line of sight {}, suspension {})",
                  los ? L"redirected" : L"NOT redirected", suspend ? L"redirected" : L"NOT redirected");
}

void RunForSeat(int seat, void* system) noexcept
{
    if (g_driver == nullptr || !SeatBuilt(seat) || !CopyFor(seat, system))
    {
        return;
    }
    Seat&                     s = g_seats[seat];
    const platform::ByteScope copy(s.copy);
    if (!copy.Held())
    {
        return;
    }
    ++s.counts.runs;
    if (!platform::CallGuarded(g_driver, system))
    {
        platform::Log(L"target cursor 2: the cursor driver for {} RAISED AN EXCEPTION", SeatName(seat));
    }
    const bool shown = CopyWidget(s) != nullptr;
    if (shown)
    {
        ++s.counts.shown;
    }
    if (shown != s.cursorSeen)
    {
        s.cursorSeen = shown;
        platform::LogDebug(L"target cursor 2: {}'s cursor {} (the game's driver, on that player's target)", SeatName(seat),
                           shown ? L"shown" : L"gone");
    }
}

void End(int seat) noexcept
{
    if (!SeatBuilt(seat) || !g_seats[seat].copy.Defined())
    {
        return;
    }
    ReleaseCopy(seat, L"the battle ended or its player left");
    g_seats[seat].copy.Clear();
    g_seats[seat].system = nullptr;
}

bool DisplayedTarget(int seat, void*& object, void*& controller) noexcept
{
    object = controller = nullptr;
    if (!SeatBuilt(seat))
    {
        return false;
    }
    const Seat& s = g_seats[seat];
    if (!s.copy.Defined() || s.copy.IsOpen() || System() != s.system)
    {
        return false;  // open: the live key is the seat's just now; the passes never run inside the targeting run
    }
    const auto key = s.copy.Value(1);
    if (key.size() < 2 * sizeof(void*))
    {
        return false;
    }
    std::memcpy(&object, key.data(), sizeof(object));
    std::memcpy(&controller, key.data() + sizeof(object), sizeof(controller));
    return object != nullptr;
}

bool HighlightName(int seat, std::uint64_t& name) noexcept
{
    name = 0;
    if (!SeatBuilt(seat))
    {
        return false;
    }
    const Seat& s = g_seats[seat];
    if (!s.copy.Defined() || s.copy.IsOpen() || System() != s.system)
    {
        return false;
    }
    constexpr std::size_t at   = facts::kOutsideIconHighlightName - facts::kOutsideIconTargetName;
    const auto            text = s.copy.Value(2);
    if (text.size() < at + sizeof(name))
    {
        return false;
    }
    std::memcpy(&name, text.data() + at, sizeof(name));
    return true;
}

void BeginPass(int column) noexcept
{
    g_hiddenCount = 0;
    if (column < 0)
    {
        return;
    }
    void* system = System();
    if (system == nullptr)
    {
        return;
    }
    // A seat's column is its place in the engine's list (hud/overlay_pass.h's map): the column's own cursor stays,
    // every other player's goes transparent.
    if (column >= 1)
    {
        void* one = nullptr;
        if (Read(static_cast<const std::uint8_t*>(system) + facts::kBattleSysCursorWidget, one))
        {
            Transparent(one);  // player 1's cursor out of the further column
        }
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        if (hud::overlay_pass::ColumnOf(seat) != column && s.copy.Defined() && system == s.system)
        {
            Transparent(CopyWidget(s));  // the seat's cursor out of another player's column
        }
    }
}

void EndPass() noexcept
{
    for (int i = 0; i < g_hiddenCount; ++i)
    {
        platform::CallGuarded(g_setColor, g_hidden[i].widget, static_cast<const float*>(g_hidden[i].color));
        g_hidden[i].widget = nullptr;
    }
    g_hiddenCount = 0;
}

}  // namespace lss::battle::target_cursor_two
