#include "battle/command_menu_two.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/player_two_camera.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>

namespace lss::battle::command_menu_two {

namespace {

using platform::Read;
using players::kSeatsBuilt;
using players::SeatBuilt;
using players::SeatName;

using MallocFn        = void* (*)(void* allocator, std::uint64_t size, std::uint32_t alignment);
using CtorFn          = void* (*)(void* controller);
using SetupFn         = std::uint64_t (*)(void* controller, void* world);
using CtrlFn          = void (*)(void* controller);
using HudModeFn       = void (*)(void* controller, std::uint8_t mode);
using RebuildFn       = std::uint64_t (*)(void* partyList, std::uint64_t a2);
using ListsFn         = void (*)(void* commandLists, std::uint64_t a2);
using MenuAllowedFn   = double (*)(void* controller, std::uint8_t on, std::uint8_t keep, std::uint8_t a4);
using InputLiveFn     = void (*)(void* controller, std::uint8_t live, std::uint64_t a3);
using NativeTickFn    = std::int64_t (*)(void* widget, const void* geometry, float deltaTime);
using SetColorFn      = void (*)(void* widget, const float* color);
using GetWorldFn      = void* (*)(void* gameMode);

/// Every scope a call on a seat's controller runs in: its focus context, the UI slot, the primary, the leader and
/// the camera operator (menu camera flavours), the UI filters' latches.
constexpr unsigned kScopes = players::scopes::All;
/// ResetAll needs no member: its layer pop and capture removal act on the focus context, its slot write on the world.
constexpr unsigned kResetScopes = players::scopes::Focus | players::scopes::UiSlot | players::scopes::Latches | players::scopes::Primary;
/// The two roots a controller owns: the command menu and the field menu (the party panel).
constexpr std::array<std::uint32_t, 2> kRoots{facts::kCommandCtrlMenuWeak, facts::kCommandCtrlFieldMenuWeak};

// Set once at install.
void**            g_gmalloc  = nullptr;
void**            g_gameMode = nullptr;
std::uintptr_t    g_slot     = 0;  // EndUISystem[19]
CtorFn            g_ctor         = nullptr;
SetupFn           g_setup        = nullptr;
CtrlFn            g_resetAll     = nullptr;
CtrlFn            g_refresh      = nullptr;
InputLiveFn       g_setInputLive = nullptr;
SetColorFn        g_setColor     = nullptr;

platform::CallRedirect  g_setupCall, g_setupJump;
std::array<platform::CallRedirect, 3> g_resetAllCalls;
platform::CallRedirect  g_rebuildCall;
platform::EntryRedirect g_hudMode, g_lists, g_menuAllowed;
platform::VTableEntry   g_nativeTickSlot;
NativeTickFn            g_nativeTick = nullptr;

// Game thread. Index = seat ([0] unused): each seat's controller and what its pass and its tick keep.
struct Seat
{
    void*        ctrl        = nullptr;  // the seat's controller, once built (kept for the game's life)
    bool         plays       = false;    // the seat's player holds a member (the session's word, each tick)
    bool         buildFailed = false;    // the allocation or the constructor failed once: no retry every tick
    alignas(16) float colorOne[kRoots.size()][4]{};  // player 1's roots' colours while transparent in this seat's pass
    bool         swapped[kRoots.size()]{};           // which pair this seat's pass swapped this frame
    bool         borrowLogged = false;
    std::int32_t ordinal      = -1;      // the controller's cached leader ordinal, as last seen
};
std::array<Seat, players::scopes::kMaxPlayers> g_seats;
bool          g_enabled       = true;
int           g_column        = hud::overlay_pass::kSingle;  // the column whose pass runs (hud/overlay_pass.h)
bool          g_skippedLogged = false;
std::uint32_t g_skipped       = 0;  // command menu NativeTicks skipped in the further columns' passes

std::uint8_t* Ctrl1() noexcept
{
    std::uint8_t* ctrl = nullptr;
    return Read(reinterpret_cast<const void*>(g_slot), ctrl) ? ctrl : nullptr;
}

std::uint8_t Byte(const void* object, std::uint32_t offset) noexcept
{
    std::uint8_t value = 0;
    Read(static_cast<const std::uint8_t*>(object) + offset, value);
    return value;
}

/// The seat's controller is built and its Setup ran.
bool ReadyOf(const Seat& seat) noexcept
{
    return seat.ctrl != nullptr && Byte(seat.ctrl, facts::kCommandCtrlSetUp) != 0;
}

/// A controller's widget (Command_C at +0x300, Status_C at +0x2F8), or null.
void* Root(const void* controller, std::uint32_t weakOffset) noexcept
{
    std::int32_t pair[2]{-1, 0};
    return controller != nullptr && Read(static_cast<const std::uint8_t*>(controller) + weakOffset, pair)
               ? players::player_two_camera::ObjectOf(pair)
               : nullptr;
}

/// The seat whose controller's command menu `widget` is, or 0 for none (the game's, or no seat's).
int SeatOfMenu(const void* widget) noexcept
{
    for (int seat = 1; widget != nullptr && seat <= kSeatsBuilt; ++seat)
    {
        if (g_seats[seat].ctrl != nullptr && widget == Root(g_seats[seat].ctrl, facts::kCommandCtrlMenuWeak))
        {
            return seat;
        }
    }
    return 0;
}

void Tint(void* widget, const float (&color)[4]) noexcept
{
    if (widget != nullptr && !platform::CallGuarded(g_setColor, widget, static_cast<const float*>(color)))
    {
        platform::LogDebug(L"command menu 2: SetColorAndOpacity faulted");
    }
}

bool Colour(const void* widget, float (&out)[4]) noexcept
{
    return widget != nullptr && Read(static_cast<const std::uint8_t*>(widget) + facts::kUserWidgetColor, out);
}

/// Alpha 0, the colour kept. A widget stays transparent until a pass gives it a colour again.
void Transparent(void* widget) noexcept
{
    alignas(16) float colour[4];
    if (Colour(widget, colour) && colour[3] != 0.0f)
    {
        colour[3] = 0.0f;
        Tint(widget, colour);
    }
}

/// The world the game mode names (for a Setup of our own).
void* World() noexcept
{
    void*      gameMode = nullptr;
    void**     vtable   = nullptr;
    GetWorldFn getWorld = nullptr;
    void*      world    = nullptr;
    return Read(g_gameMode, gameMode) && gameMode != nullptr && Read(gameMode, vtable) &&
                   Read(reinterpret_cast<std::uint8_t*>(vtable) + facts::kGameModeGetWorldSlot, getWorld) &&
                   platform::CallGuarded(getWorld, world, gameMode)
               ? world
               : nullptr;
}

/// Setup on the seat's controller inside its scopes. False if the scopes could not open or the call faulted.
bool SetupSeat(int seat, void* world) noexcept
{
    players::scopes::Scope scope(seat, kScopes);
    std::uint64_t          result = 0;
    if (!scope.Held())
    {
        platform::LogDebug(L"command menu 2: Setup skipped ({}'s scopes could not open)", SeatName(seat));
        return false;
    }
    if (!platform::CallGuarded(g_setup, result, g_seats[seat].ctrl, world))
    {
        platform::Log(L"command menu 2: Setup on {}'s controller RAISED AN EXCEPTION", SeatName(seat));
        return false;
    }
    return true;
}

/// Builds the seat's controller with the game's three calls (the ctor registers it with the GC referencer, so it is
/// never freed; the seat's later players reuse it).
bool Build(int seat) noexcept
{
    void*    allocator = nullptr;
    void**   vtable    = nullptr;
    MallocFn malloc    = nullptr;
    void*    memory    = nullptr;
    void*    built     = nullptr;
    if (!Read(g_gmalloc, allocator) || allocator == nullptr || !Read(allocator, vtable) ||
        !Read(reinterpret_cast<std::uint8_t*>(vtable) + 0x10, malloc) ||
        !platform::CallGuarded(malloc, memory, allocator, std::uint64_t{facts::kCommandCtrlSize}, std::uint32_t{0}) ||
        memory == nullptr || !platform::CallGuarded(g_ctor, built, memory) || built == nullptr)
    {
        platform::Log(L"command menu 2: {}'s controller could not be allocated or constructed; {} has no menu", SeatName(seat),
                      SeatName(seat));
        return false;
    }
    g_seats[seat].ctrl = built;
    players::scopes::BindCommandController(seat, built);
    platform::Log(L"command menu 2: {}'s controller built ({:#x})", SeatName(seat), reinterpret_cast<std::uintptr_t>(built));
    return true;
}

/// `fn(seat)` for every built seat whose controller is set up (the game's call is repeated on each).
template <class Fn>
void ForEachReady(Fn&& fn) noexcept
{
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (ReadyOf(g_seats[seat]))
        {
            fn(seat);
        }
    }
}

// --- The game's calls, each made again on every seat's controller -------------------------------------------------

std::uint64_t Setup(void* controller, void* world)
{
    const std::uint64_t result = g_setup(controller, world);
    if (controller == Ctrl1() && g_enabled)
    {
        for (int seat = 1; seat <= kSeatsBuilt; ++seat)
        {
            if (g_seats[seat].ctrl != nullptr && g_seats[seat].plays)
            {
                platform::LogDebug(L"command menu 2: the game's Setup, repeated on {}'s controller", SeatName(seat));
                SetupSeat(seat, world);
            }
        }
    }
    return result;
}

void ResetAll(void* controller)
{
    g_resetAll(controller);
    if (controller != Ctrl1() || !g_enabled)
    {
        return;
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (g_seats[seat].ctrl == nullptr)
        {
            continue;
        }
        players::scopes::Scope scope(seat, kResetScopes);
        if (scope.Held())  // unbound (the seat has no player): the controller was already reset at the leave
        {
            platform::LogDebug(L"command menu 2: the game's ResetAll, repeated on {}'s controller", SeatName(seat));
            platform::CallGuarded(g_resetAll, g_seats[seat].ctrl);
        }
    }
}

void SetHudMode(void* controller, std::uint8_t mode)
{
    reinterpret_cast<HudModeFn>(g_hudMode.Original())(controller, mode);
    if (controller != Ctrl1() || !g_enabled)
    {
        return;
    }
    ForEachReady([mode](int seat) {
        players::scopes::Scope scope(seat, kScopes);
        if (scope.Held())
        {
            platform::LogDebug(L"command menu 2: battle HUD mode {} on {}'s controller", mode, SeatName(seat));
            platform::CallGuarded(reinterpret_cast<HudModeFn>(g_hudMode.Original()), g_seats[seat].ctrl, mode);
        }
    });
}

std::uint64_t Rebuild(void* partyList, std::uint64_t a2)
{
    const std::uint64_t result = reinterpret_cast<RebuildFn>(g_rebuildCall.Original())(partyList, a2);
    if (Ctrl1() == nullptr || partyList != Ctrl1() + facts::kCommandCtrlPartyList || !g_enabled)
    {
        return result;
    }
    ForEachReady([](int seat) {
        players::scopes::Scope scope(seat, kScopes);
        if (scope.Held())
        {
            platform::LogDebug(L"command menu 2: the party panel rebuilt on {}'s controller", SeatName(seat));
            platform::CallGuarded(g_refresh, g_seats[seat].ctrl);  // its own pass through here: a different list, the game's call only
        }
    });
    return result;
}

void RebuildLists(void* commandLists, std::uint64_t a2)
{
    reinterpret_cast<ListsFn>(g_lists.Original())(commandLists, a2);
    if (Ctrl1() == nullptr || commandLists != Ctrl1() + facts::kCommandCtrlCommandLists || !g_enabled)
    {
        return;
    }
    ForEachReady([a2](int seat) {
        players::scopes::Scope scope(seat, kScopes);
        if (scope.Held())
        {
            platform::CallGuarded(reinterpret_cast<ListsFn>(g_lists.Original()),
                                  static_cast<void*>(static_cast<std::uint8_t*>(g_seats[seat].ctrl) + facts::kCommandCtrlCommandLists), a2);
        }
    });
}

double SetMenuAllowed(void* controller, std::uint8_t on, std::uint8_t keep, std::uint8_t a4)
{
    const double result = reinterpret_cast<MenuAllowedFn>(g_menuAllowed.Original())(controller, on, keep, a4);
    if (controller != Ctrl1() || !g_enabled)
    {
        return result;
    }
    ForEachReady([on, keep, a4](int seat) {
        players::scopes::Scope scope(seat, kScopes);
        if (scope.Held())
        {
            double ignored = 0.0;
            platform::CallGuarded(reinterpret_cast<MenuAllowedFn>(g_menuAllowed.Original()), ignored, g_seats[seat].ctrl, on, keep, a4);
        }
    });
    return result;
}

/// UEndFieldCommandMenu::NativeTick: once per frame per command menu. The further columns' passes (the first paints)
/// are skipped for every command menu; a seat's own menu ticks inside that seat's scopes.
std::int64_t NativeTick(void* widget, const void* geometry, float deltaTime)
{
    if (g_column >= 1 && g_enabled)
    {
        ++g_skipped;
        if (!g_skippedLogged)
        {
            g_skippedLogged = true;
            platform::Log(L"command menu 2: a command menu's key-state advance in a further column's pass was skipped (one advance per frame)");
        }
        return 0;
    }
    if (const int seat = g_enabled ? SeatOfMenu(widget) : 0; seat != 0)
    {
        players::scopes::Scope scope(seat, kScopes);
        return g_nativeTick(widget, geometry, deltaTime);
    }
    return g_nativeTick(widget, geometry, deltaTime);
}

template <class Fn>
bool FindInto(const platform::GameImage& image, const platform::FunctionFact& fact, Fn& out) noexcept
{
    const auto address = platform::FindFunction(image, fact);
    out                = address ? reinterpret_cast<Fn>(*address) : nullptr;
    return out != nullptr;
}

/// The bytes that gate each controller's input poll (RE-ko-leader-input.md §3.1): menu allowed (+0x3D5), input
/// live (+0x420), the battle HUD mode (+0x348), the cached leader ordinal (+0x28); and the game state's pause counter.
std::wstring GatesReading() noexcept
{
    const auto fields = [](const void* ctrl) {
        if (ctrl == nullptr)
        {
            return std::wstring(L"none");
        }
        std::int32_t ordinal = -1;
        Read(static_cast<const std::uint8_t*>(ctrl) + facts::kCommandCtrlLeaderOrdinal, ordinal);
        return std::format(L"allowed {} live {} hud {} ordinal {}", Byte(ctrl, facts::kCommandCtrlMenuAllowed),
                           Byte(ctrl, facts::kCommandCtrlInputLive), Byte(ctrl, facts::kCommandCtrlBattleHudMode), ordinal);
    };
    void*         gameMode = nullptr;
    std::uint8_t* state    = nullptr;
    std::int32_t  paused   = -1;
    if (Read(g_gameMode, gameMode) && gameMode != nullptr && Read(static_cast<std::uint8_t*>(gameMode) + facts::kGameModeGameState, state) &&
        state != nullptr)
    {
        Read(state + facts::kGameStatePauseCounter, paused);
    }
    std::wstring text = std::format(L"player 1 {}", fields(Ctrl1()));
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        text += std::format(L"; {} {}", SeatName(seat), fields(g_seats[seat].ctrl));
    }
    return text + std::format(L"; pause counter {}", paused);
}

/// Each controller's cached leader ordinal and member count, and the party's slot types.
std::wstring LeaderReading() noexcept
{
    const auto fields = [](const void* ctrl) {
        std::int32_t ordinal = -1, count = -1;
        if (ctrl != nullptr)
        {
            Read(static_cast<const std::uint8_t*>(ctrl) + facts::kCommandCtrlLeaderOrdinal, ordinal);
            Read(static_cast<const std::uint8_t*>(ctrl) + facts::kCommandCtrlMemberCount, count);
        }
        return std::format(L"ordinal {} of {}", ordinal, count);
    };
    std::uint8_t slots[3]{9, 9, 9};
    Read(reinterpret_cast<const void*>(players::scopes::PartyManagerAddress() + facts::kPartySlotTypes), slots);
    std::wstring text = std::format(L"player 1 {}", fields(Ctrl1()));
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        text += std::format(L", {} {}", SeatName(seat), fields(g_seats[seat].ctrl));
    }
    return text + std::format(L"; party slot types {} {} {}", slots[0], slots[1], slots[2]);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_nativeTickSlot.Active())
    {
        return false;
    }
    const auto gmalloc  = image.Rebase(facts::kGMallocGlobal, sizeof(void*));
    const auto gameMode = image.Rebase(facts::kGameModeGlobal, sizeof(void*));
    const auto slot     = image.Rebase(facts::kCommandCtrlSlotGlobal, sizeof(void*));
    const auto hudMode  = platform::FindFunction(image, facts::kCommandCtrlSetHudMode);
    const auto lists    = platform::FindFunction(image, facts::kCommandListsRebuild);
    const auto allowed  = platform::FindFunction(image, facts::kCommandCtrlSetMenuAllowed);
    const auto rebuild  = platform::FindFunction(image, facts::kPartyListRebuild);
    const auto setColor = image.Rebase(facts::kSetColorAndOpacity);
    const auto setupCall = image.Rebase(facts::kCommandCtrlSetupCall, 5);
    const auto setupJump = image.Rebase(facts::kCommandCtrlSetupTailJump, 5);
    const auto rebuildCall = image.Rebase(facts::kPartyListRebuildCall, 5);
    std::array<std::optional<std::uintptr_t>, 3> resetCalls;
    for (std::size_t i = 0; i < resetCalls.size(); ++i)
    {
        resetCalls[i] = image.Rebase(facts::kCommandCtrlResetAllCalls[i], 5);
    }
    if (!gmalloc || !gameMode || !slot || !hudMode || !lists || !allowed || !rebuild || !setColor || !setupCall ||
        !setupJump || !rebuildCall || !resetCalls[0] || !resetCalls[1] || !resetCalls[2] ||
        !FindInto(image, facts::kCommandCtrlCtor, g_ctor) || !FindInto(image, facts::kCommandCtrlSetup, g_setup) ||
        !FindInto(image, facts::kCommandCtrlResetAll, g_resetAll) || !FindInto(image, facts::kCommandCtrlRefresh, g_refresh) ||
        !FindInto(image, facts::kCommandCtrlSetInputLive, g_setInputLive) ||
        !FindInto(image, facts::kCommandMenuNativeTick.target, g_nativeTick))
    {
        platform::Log(L"command menu 2: an engine check failed; the seats have no command menu");
        return false;
    }
    g_gmalloc  = reinterpret_cast<void**>(*gmalloc);
    g_gameMode = reinterpret_cast<void**>(*gameMode);
    g_slot     = *slot;
    g_setColor = reinterpret_cast<SetColorFn>(*setColor);

    const auto setupTarget = reinterpret_cast<std::uintptr_t>(g_setup);
    const auto resetTarget = reinterpret_cast<std::uintptr_t>(g_resetAll);
    const bool installed =
        g_setupCall.Install(*setupCall, setupTarget, reinterpret_cast<std::uintptr_t>(&Setup)) &&
        g_setupJump.InstallTailJump(*setupJump, setupTarget, reinterpret_cast<std::uintptr_t>(&Setup)) &&
        g_resetAllCalls[0].Install(*resetCalls[0], resetTarget, reinterpret_cast<std::uintptr_t>(&ResetAll)) &&
        g_resetAllCalls[1].Install(*resetCalls[1], resetTarget, reinterpret_cast<std::uintptr_t>(&ResetAll)) &&
        g_resetAllCalls[2].Install(*resetCalls[2], resetTarget, reinterpret_cast<std::uintptr_t>(&ResetAll)) &&
        g_rebuildCall.Install(*rebuildCall, *rebuild, reinterpret_cast<std::uintptr_t>(&Rebuild)) &&
        g_hudMode.Install(*hudMode, std::span(facts::kCommandCtrlSetHudModeEntry.data(), facts::kHomeStoreBytes),
                          reinterpret_cast<std::uintptr_t>(&SetHudMode)) &&
        g_lists.Install(*lists, std::span(facts::kCommandListsRebuildEntry.data(), facts::kHomeStoreBytes),
                        reinterpret_cast<std::uintptr_t>(&RebuildLists)) &&
        g_menuAllowed.Install(*allowed, std::span(facts::kCommandCtrlSetMenuAllowedEntry.data(), facts::kHomeStoreBytes),
                              reinterpret_cast<std::uintptr_t>(&SetMenuAllowed)) &&
        platform::ReplaceSlot(image, facts::kCommandMenuNativeTick, reinterpret_cast<std::uintptr_t>(&NativeTick), g_nativeTickSlot);
    if (!installed)
    {
        Remove();
        platform::Log(L"command menu 2: a redirect could not be placed; the seats have no command menu");
        return false;
    }
    control::Register({.name  = L"menu-two",
                       .shape = control::Shape::OnOff,
                       .help  = L"each seat's own command menu follows the game's (battle/command_menu_two.h; on)",
                       .set   = [](const control::Value& value) {
                           g_enabled = std::get<bool>(value);
                           platform::Log(L"command menu 2 {}", g_enabled ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"menu-two.leader", .shape = control::Shape::Text,
                       .help = L"each command controller's cached leader ordinal and member count, and the party's slot types "
                               L"(the poll reads no input while the member at its ordinal is KO'd)",
                       .get  = [] { return control::Value{LeaderReading()}; }});
    control::Register({.name = L"menu-two.gates", .shape = control::Shape::Text,
                       .help = L"the bytes gating each controller's input poll (menu allowed, input live, battle HUD mode, "
                               L"cached leader ordinal) and the game state's pause counter",
                       .get  = [] { return control::Value{GatesReading()}; }});
    control::Register({.name = L"menu-two.state", .shape = control::Shape::Word, .words = {L"none", L"built", L"ready"},
                       .help = L"player 2's command controller: none, built, or ready (set up); seat 1 (plan/seats.md step 7 generalises the readings)",
                       .get  = [] {
                           const Seat& seat = g_seats[1];
                           return control::Value{std::wstring(seat.ctrl == nullptr ? L"none" : ReadyOf(seat) ? L"ready" : L"built")};
                       }});
    platform::Log(L"command menu 2: installed; the game's controller calls are repeated on each seat's");
    return true;
}

void Remove() noexcept
{
    g_nativeTickSlot.Restore();
    g_menuAllowed.Restore();
    g_lists.Restore();
    g_hudMode.Restore();
    g_rebuildCall.Restore();
    for (auto& call : g_resetAllCalls)
    {
        call.Restore();
    }
    g_setupJump.Restore();
    g_setupCall.Restore();
}

void Tick(int seat, bool seatPlays) noexcept
{
    if (!SeatBuilt(seat))
    {
        return;
    }
    Seat& s = g_seats[seat];
    s.plays = seatPlays;
    if (!g_enabled || !s.plays || g_ctor == nullptr || s.buildFailed)
    {
        return;
    }
    const wchar_t* name  = SeatName(seat);
    std::uint8_t*  ctrl1 = Ctrl1();
    if (ctrl1 == nullptr || Byte(ctrl1, facts::kCommandCtrlSetUp) == 0)
    {
        return;  // the game's own controller is not set up yet (a load, the title): neither is the seat's
    }
    if (s.ctrl == nullptr && !Build(seat))
    {
        s.buildFailed = true;  // once: no retry every tick
        return;
    }
    if (players::scopes::CommandController(seat) == nullptr)
    {
        players::scopes::BindCommandController(seat, s.ctrl);  // the seat's new player: a fresh focus copy
    }
    if (!ReadyOf(s))
    {
        void* world = World();
        if (world != nullptr && SetupSeat(seat, world))
        {
            if (ReadyOf(s))
            {
                // Its roots start transparent; they show only in the seat's own pass.
                for (const std::uint32_t root : kRoots)
                {
                    Transparent(Root(s.ctrl, root));
                }
                s.borrowLogged = false;
                platform::Log(L"command menu 2: {}'s controller set up (its menu {} and panel {} in {}'s context; path set {})", name,
                              Root(s.ctrl, facts::kCommandCtrlMenuWeak) != nullptr ? L"built" : L"MISSING",
                              Root(s.ctrl, facts::kCommandCtrlFieldMenuWeak) != nullptr ? L"built" : L"MISSING", name,
                              players::scopes::FocusPathSet(seat) ? L"yes" : L"NO");
            }
            else
            {
                platform::LogDebug(L"command menu 2: {}'s Setup deferred by the game (pending {})", name, Byte(s.ctrl, facts::kCommandCtrlSetupPending));
            }
        }
        return;
    }
    // The ordinal the controller's own tick caches (its poll stops while that member is KO'd): a change is logged.
    std::int32_t ordinal = -1;
    if (Read(static_cast<const std::uint8_t*>(s.ctrl) + facts::kCommandCtrlLeaderOrdinal, ordinal) && ordinal != s.ordinal)
    {
        s.ordinal = ordinal;
        platform::LogDebug(L"command menu 2: {}'s controller now caches leader ordinal {} ({}'s member type {}, {})", name, ordinal, name,
                           players::scopes::MemberType(seat), LeaderReading());
    }
    // The battle HUD mode the game set on its controller while the seat had no member (a battle begun in a cutscene
    // that took the member): the setter's repeat needs a set-up controller, so the mode is caught up here through the
    // same setter (the skill's lesson: the second instance misses events while its player is out; replay them).
    const std::uint8_t hudMode = Byte(ctrl1, facts::kCommandCtrlBattleHudMode);
    if (Byte(s.ctrl, facts::kCommandCtrlBattleHudMode) != hudMode)
    {
        players::scopes::Scope scope(seat, kScopes);
        if (scope.Held())
        {
            platform::LogDebug(L"command menu 2: battle HUD mode {} caught up on {}'s controller (set while {} had no member)", hudMode, name, name);
            platform::CallGuarded(reinterpret_cast<HudModeFn>(g_hudMode.Original()), s.ctrl, hudMode);
        }
    }
    // The two bytes the game writes directly on its own controller.
    const std::uint8_t allowed = Byte(ctrl1, facts::kCommandCtrlMenuAllowed);
    const std::uint8_t live    = Byte(ctrl1, facts::kCommandCtrlInputLive);
    if (Byte(s.ctrl, facts::kCommandCtrlMenuAllowed) != allowed)
    {
        // Off closes both menus' polls (the game's own gate 4 of RE-ko-leader-input.md §3.1): a script or event
        // that closes player 1's menu closes the seat's too, so each change is logged.
        platform::LogDebug(L"command menu 2: menu allowed {} on {}'s controller (copied from the game's)", allowed, name);
        platform::WriteGuarded(static_cast<std::uint8_t*>(s.ctrl) + facts::kCommandCtrlMenuAllowed, &allowed, 1);
    }
    if (Byte(s.ctrl, facts::kCommandCtrlInputLive) != live)
    {
        players::scopes::Scope scope(seat, kScopes);
        if (scope.Held())
        {
            platform::LogDebug(L"command menu 2: input live {} on {}'s controller", live, name);
            platform::CallGuarded(g_setInputLive, s.ctrl, live, std::uint64_t{0});
        }
    }
}

void OnLeave(int seat) noexcept
{
    if (!SeatBuilt(seat))
    {
        return;
    }
    Seat& s   = g_seats[seat];
    s.plays   = false;
    s.ordinal = -1;  // the seat's next player's ordinal is logged afresh
    if (s.ctrl == nullptr)
    {
        return;
    }
    // ResetAll inside the seat's own context pops its layers from its own copy and clears its set-up bytes, so the
    // seat's next player's Setup pushes them into a fresh context.
    if (ReadyOf(s))
    {
        players::scopes::Scope scope(seat, kResetScopes);
        if (scope.Held())
        {
            platform::CallGuarded(g_resetAll, s.ctrl);
        }
    }
    players::scopes::BindCommandController(seat, nullptr);
    platform::LogDebug(L"command menu 2: {}'s controller reset for the seat's next player", SeatName(seat));
}

void* Controller(int seat) noexcept
{
    return SeatBuilt(seat) ? g_seats[seat].ctrl : nullptr;
}

bool Ready(int seat) noexcept
{
    return SeatBuilt(seat) && ReadyOf(g_seats[seat]);
}

void BeginPass(int column) noexcept
{
    g_column = column;
    // A further column is its seat's (hud/overlay_pass.h): that seat's menu shows in it, the other seats' stay
    // transparent (nothing gives them a colour outside their own pass).
    const int seat = column;
    if (column < 1 || !Ready(seat) || !g_enabled)
    {
        return;
    }
    // The seat's roots take player 1's roots' colours for the pass (the game's fades on player 1's carry over,
    // and a root that outlives a leave keeps no stale colour of its own); player 1's go transparent.
    Seat& s = g_seats[seat];
    for (std::size_t i = 0; i < kRoots.size(); ++i)
    {
        void* one = Root(Ctrl1(), kRoots[i]);
        void* own = Root(s.ctrl, kRoots[i]);
        s.swapped[i] = one != nullptr && own != nullptr && Colour(one, s.colorOne[i]);
        if (!s.swapped[i])
        {
            continue;
        }
        Tint(own, s.colorOne[i]);
        Transparent(one);
    }
    if (!s.borrowLogged && (s.swapped[0] || s.swapped[1]))
    {
        s.borrowLogged = true;
        platform::LogDebug(L"command menu 2: {}'s roots take player 1's colours in its pass (menu alpha {:.2f}, panel alpha {:.2f})", SeatName(seat),
                           s.swapped[0] ? s.colorOne[0][3] : -1.0f, s.swapped[1] ? s.colorOne[1][3] : -1.0f);
    }
}

void EndPass() noexcept
{
    if (SeatBuilt(g_column))
    {
        Seat& s = g_seats[g_column];
        for (std::size_t i = 0; i < kRoots.size(); ++i)
        {
            if (s.swapped[i])
            {
                s.swapped[i] = false;
                Tint(Root(Ctrl1(), kRoots[i]), s.colorOne[i]);
                Transparent(Root(s.ctrl, kRoots[i]));
            }
        }
    }
    g_column = hud::overlay_pass::kSingle;
}

}  // namespace lss::battle::command_menu_two
