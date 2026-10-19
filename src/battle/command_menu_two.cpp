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

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>

namespace lss::battle::command_menu_two {

namespace {

using platform::Read;

using hud::overlay_pass::Pass;

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

constexpr int kPlayer = 1;  // seat index of player 2 (players/scopes.h)
/// Every scope a call on player 2's controller runs in: its focus context, the UI slot, the primary, the leader and
/// the camera operator (menu camera flavours), the UI filters' latches.
constexpr unsigned kScopes = players::scopes::All;
/// ResetAll needs no member: its layer pop and capture removal act on the focus context, its slot write on the world.
constexpr unsigned kResetScopes = players::scopes::Focus | players::scopes::UiSlot | players::scopes::Latches | players::scopes::Primary;

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

// Game thread.
void* g_ctrl      = nullptr;  // player 2's controller, once built (kept for the game's life)
bool  g_plays     = false;
bool  g_enabled   = true;
Pass  g_pass      = Pass::Single;
/// The two roots a controller owns: the command menu and the field menu (the party panel).
constexpr std::array<std::uint32_t, 2> kRoots{facts::kCommandCtrlMenuWeak, facts::kCommandCtrlFieldMenuWeak};
alignas(16) float g_colorOne[kRoots.size()][4]{};  // player 1's roots' colours while transparent in player 2's pass
bool              g_swapped[kRoots.size()]{};      // which pair player 2's pass swapped this frame
bool              g_borrowLogged = false;
bool  g_skippedLogged = false;
std::int32_t g_ordinalTwo = -1;  // player 2's controller's cached leader ordinal, as last seen
std::uint32_t g_skipped = 0;  // command menu NativeTicks skipped in player 2's pass

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

/// A controller's widget (Command_C at +0x300, Status_C at +0x2F8), or null.
void* Root(const void* controller, std::uint32_t weakOffset) noexcept
{
    std::int32_t pair[2]{-1, 0};
    return controller != nullptr && Read(static_cast<const std::uint8_t*>(controller) + weakOffset, pair)
               ? players::player_two_camera::ObjectOf(pair)
               : nullptr;
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

/// Setup on player 2's controller inside its scopes. False if the scopes could not open or the call faulted.
bool SetupTwo(void* world) noexcept
{
    players::scopes::Scope scope(kPlayer, kScopes);
    std::uint64_t          result = 0;
    if (!scope.Held())
    {
        platform::LogDebug(L"command menu 2: Setup skipped (player 2's scopes could not open)");
        return false;
    }
    if (!platform::CallGuarded(g_setup, result, g_ctrl, world))
    {
        platform::Log(L"command menu 2: Setup on player 2's controller RAISED AN EXCEPTION");
        return false;
    }
    return true;
}

/// Builds player 2's controller with the game's three calls (the ctor registers it with the GC referencer, so it
/// is never freed; a later player 2 reuses it).
bool Build() noexcept
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
        platform::Log(L"command menu 2: player 2's controller could not be allocated or constructed; player 2 has no menu");
        return false;
    }
    g_ctrl = built;
    players::scopes::BindCommandController(kPlayer, g_ctrl);
    platform::Log(L"command menu 2: player 2's controller built ({:#x})", reinterpret_cast<std::uintptr_t>(g_ctrl));
    return true;
}

// --- The game's calls, each made again on player 2's controller ---------------------------------------------------

std::uint64_t Setup(void* controller, void* world)
{
    const std::uint64_t result = g_setup(controller, world);
    if (controller == Ctrl1() && g_ctrl != nullptr && g_plays && g_enabled)
    {
        platform::LogDebug(L"command menu 2: the game's Setup, repeated on player 2's controller");
        SetupTwo(world);
    }
    return result;
}

void ResetAll(void* controller)
{
    g_resetAll(controller);
    if (controller == Ctrl1() && g_ctrl != nullptr && g_enabled)
    {
        players::scopes::Scope scope(kPlayer, kResetScopes);
        if (scope.Held())  // unbound (no player 2): the controller was already reset at the leave
        {
            platform::LogDebug(L"command menu 2: the game's ResetAll, repeated on player 2's controller");
            platform::CallGuarded(g_resetAll, g_ctrl);
        }
    }
}

void SetHudMode(void* controller, std::uint8_t mode)
{
    reinterpret_cast<HudModeFn>(g_hudMode.Original())(controller, mode);
    if (controller == Ctrl1() && Ready() && g_enabled)
    {
        players::scopes::Scope scope(kPlayer, kScopes);
        if (scope.Held())
        {
            platform::LogDebug(L"command menu 2: battle HUD mode {} on player 2's controller", mode);
            platform::CallGuarded(reinterpret_cast<HudModeFn>(g_hudMode.Original()), g_ctrl, mode);
        }
    }
}

std::uint64_t Rebuild(void* partyList, std::uint64_t a2)
{
    const std::uint64_t result = reinterpret_cast<RebuildFn>(g_rebuildCall.Original())(partyList, a2);
    if (Ctrl1() != nullptr && partyList == Ctrl1() + facts::kCommandCtrlPartyList && Ready() && g_enabled)
    {
        players::scopes::Scope scope(kPlayer, kScopes);
        if (scope.Held())
        {
            platform::LogDebug(L"command menu 2: the party panel rebuilt on player 2's controller");
            platform::CallGuarded(g_refresh, g_ctrl);  // its own pass through here: a different list, the game's call only
        }
    }
    return result;
}

void RebuildLists(void* commandLists, std::uint64_t a2)
{
    reinterpret_cast<ListsFn>(g_lists.Original())(commandLists, a2);
    if (Ctrl1() != nullptr && commandLists == Ctrl1() + facts::kCommandCtrlCommandLists && Ready() && g_enabled)
    {
        players::scopes::Scope scope(kPlayer, kScopes);
        if (scope.Held())
        {
            platform::CallGuarded(reinterpret_cast<ListsFn>(g_lists.Original()),
                                  static_cast<void*>(static_cast<std::uint8_t*>(g_ctrl) + facts::kCommandCtrlCommandLists), a2);
        }
    }
}

double SetMenuAllowed(void* controller, std::uint8_t on, std::uint8_t keep, std::uint8_t a4)
{
    const double result = reinterpret_cast<MenuAllowedFn>(g_menuAllowed.Original())(controller, on, keep, a4);
    if (controller == Ctrl1() && Ready() && g_enabled)
    {
        players::scopes::Scope scope(kPlayer, kScopes);
        if (scope.Held())
        {
            double ignored = 0.0;
            platform::CallGuarded(reinterpret_cast<MenuAllowedFn>(g_menuAllowed.Original()), ignored, g_ctrl, on, keep, a4);
        }
    }
    return result;
}

/// UEndFieldCommandMenu::NativeTick: once per frame per command menu. Player 2's pass (the first paint) is skipped
/// for every command menu; player 2's own menu ticks inside player 2's scopes.
std::int64_t NativeTick(void* widget, const void* geometry, float deltaTime)
{
    if (g_pass == Pass::PlayerTwo && g_enabled)
    {
        ++g_skipped;
        if (!g_skippedLogged)
        {
            g_skippedLogged = true;
            platform::Log(L"command menu 2: a command menu's key-state advance in player 2's pass was skipped (one advance per frame)");
        }
        return 0;
    }
    if (g_ctrl != nullptr && widget == Root(g_ctrl, facts::kCommandCtrlMenuWeak) && g_enabled)
    {
        players::scopes::Scope scope(kPlayer, kScopes);
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
    return std::format(L"ctrl1 {}, ctrl2 {}; party slot types {} {} {}", fields(Ctrl1()), fields(g_ctrl), slots[0], slots[1], slots[2]);
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
        platform::Log(L"command menu 2: an engine check failed; player 2 has no command menu");
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
        platform::Log(L"command menu 2: a redirect could not be placed; player 2 has no command menu");
        return false;
    }
    control::Register({.name  = L"menu-two",
                       .shape = control::Shape::OnOff,
                       .help  = L"player 2's own command menu follows the game's (battle/command_menu_two.h; on)",
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
    control::Register({.name = L"menu-two.state", .shape = control::Shape::Word, .words = {L"none", L"built", L"ready"},
                       .help = L"player 2's command controller: none, built, or ready (set up)",
                       .get  = [] { return control::Value{std::wstring(g_ctrl == nullptr ? L"none" : Ready() ? L"ready" : L"built")}; }});
    platform::Log(L"command menu 2: installed; the game's controller calls are repeated on player 2's");
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

void Tick(bool playerTwoPlays) noexcept
{
    g_plays = playerTwoPlays;
    if (!g_enabled || !g_plays || g_ctor == nullptr)
    {
        return;
    }
    std::uint8_t* ctrl1 = Ctrl1();
    if (ctrl1 == nullptr || Byte(ctrl1, facts::kCommandCtrlSetUp) == 0)
    {
        return;  // the game's own controller is not set up yet (a load, the title): neither is player 2's
    }
    if (g_ctrl == nullptr && !Build())
    {
        g_ctor = nullptr;  // once: no retry every tick
        return;
    }
    if (players::scopes::CommandController(kPlayer) == nullptr)
    {
        players::scopes::BindCommandController(kPlayer, g_ctrl);  // a new player 2: a fresh focus copy
    }
    if (!Ready())
    {
        void* world = World();
        if (world != nullptr && SetupTwo(world))
        {
            if (Ready())
            {
                // Its roots start transparent; they show only in player 2's pass.
                for (const std::uint32_t root : kRoots)
                {
                    Transparent(Root(g_ctrl, root));
                }
                g_borrowLogged = false;
                platform::Log(L"command menu 2: player 2's controller set up (its menu {} and panel {} in player 2's context; path set {})",
                              Root(g_ctrl, facts::kCommandCtrlMenuWeak) != nullptr ? L"built" : L"MISSING",
                              Root(g_ctrl, facts::kCommandCtrlFieldMenuWeak) != nullptr ? L"built" : L"MISSING",
                              players::scopes::FocusPathSet(kPlayer) ? L"yes" : L"NO");
            }
            else
            {
                platform::LogDebug(L"command menu 2: Setup deferred by the game (pending {})", Byte(g_ctrl, facts::kCommandCtrlSetupPending));
            }
        }
        return;
    }
    // The ordinal the controller's own tick caches (its poll stops while that member is KO'd): a change is logged.
    std::int32_t ordinal = -1;
    if (Read(static_cast<const std::uint8_t*>(g_ctrl) + facts::kCommandCtrlLeaderOrdinal, ordinal) && ordinal != g_ordinalTwo)
    {
        g_ordinalTwo = ordinal;
        platform::LogDebug(L"command menu 2: player 2's controller now caches leader ordinal {} (player 2's member type {}, {})",
                           ordinal, players::scopes::MemberType(kPlayer), LeaderReading());
    }
    // The two bytes the game writes directly on its own controller.
    const std::uint8_t allowed = Byte(ctrl1, facts::kCommandCtrlMenuAllowed);
    const std::uint8_t live    = Byte(ctrl1, facts::kCommandCtrlInputLive);
    if (Byte(g_ctrl, facts::kCommandCtrlMenuAllowed) != allowed)
    {
        platform::WriteGuarded(static_cast<std::uint8_t*>(g_ctrl) + facts::kCommandCtrlMenuAllowed, &allowed, 1);
    }
    if (Byte(g_ctrl, facts::kCommandCtrlInputLive) != live)
    {
        players::scopes::Scope scope(kPlayer, kScopes);
        if (scope.Held())
        {
            platform::LogDebug(L"command menu 2: input live {} on player 2's controller", live);
            platform::CallGuarded(g_setInputLive, g_ctrl, live, std::uint64_t{0});
        }
    }
}

void OnLeave() noexcept
{
    g_plays = false;
    if (g_ctrl == nullptr)
    {
        return;
    }
    // ResetAll inside player 2's own context pops its layers from its own copy and clears its set-up bytes, so the
    // next player 2's Setup pushes them into a fresh context.
    if (Ready())
    {
        players::scopes::Scope scope(kPlayer, kResetScopes);
        if (scope.Held())
        {
            platform::CallGuarded(g_resetAll, g_ctrl);
        }
    }
    players::scopes::BindCommandController(kPlayer, nullptr);
    platform::LogDebug(L"command menu 2: player 2's controller reset for the next player 2");
}

void* Controller() noexcept
{
    return g_ctrl;
}

bool Ready() noexcept
{
    return g_ctrl != nullptr && Byte(g_ctrl, facts::kCommandCtrlSetUp) != 0;
}

void BeginPass(Pass pass) noexcept
{
    g_pass = pass;
    if (pass != Pass::PlayerTwo || !Ready() || !g_enabled)
    {
        return;
    }
    // Player 2's roots take player 1's roots' colours for the pass (the game's fades on player 1's carry over,
    // and a root that outlives a leave keeps no stale colour of its own); player 1's go transparent.
    for (std::size_t i = 0; i < kRoots.size(); ++i)
    {
        void* one = Root(Ctrl1(), kRoots[i]);
        void* two = Root(g_ctrl, kRoots[i]);
        g_swapped[i] = one != nullptr && two != nullptr && Colour(one, g_colorOne[i]);
        if (!g_swapped[i])
        {
            continue;
        }
        Tint(two, g_colorOne[i]);
        Transparent(one);
    }
    if (!g_borrowLogged && (g_swapped[0] || g_swapped[1]))
    {
        g_borrowLogged = true;
        platform::LogDebug(L"command menu 2: player 2's roots take player 1's colours in its pass (menu alpha {:.2f}, panel alpha {:.2f})",
                           g_swapped[0] ? g_colorOne[0][3] : -1.0f, g_swapped[1] ? g_colorOne[1][3] : -1.0f);
    }
}

void EndPass() noexcept
{
    if (g_pass == Pass::PlayerTwo)
    {
        for (std::size_t i = 0; i < kRoots.size(); ++i)
        {
            if (g_swapped[i])
            {
                g_swapped[i] = false;
                Tint(Root(Ctrl1(), kRoots[i]), g_colorOne[i]);
                Transparent(Root(g_ctrl, kRoots[i]));
            }
        }
    }
    g_pass = Pass::Single;
}

}  // namespace lss::battle::command_menu_two
