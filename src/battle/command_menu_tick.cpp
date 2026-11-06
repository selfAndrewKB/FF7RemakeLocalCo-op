#include "battle/command_menu_tick.h"

#include "battle/arena_warp.h"
#include "battle/command_menu_rules.h"
#include "battle/command_menu.h"
#include "battle/targeting.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/pad_buttons.h"
#include "players/seat_camera.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace lss::battle::command_menu_tick {

namespace {

using platform::Read;
using players::kSeatsBuilt;
using players::SeatBuilt;
using players::SeatName;

using TickFn        = void (*)(void* controller, float deltaTime);
using SetSlotFn     = std::int64_t (*)(std::uint8_t slot, float value);
using InputBufferFn = void (*)(void* component, float deltaTime);

// Set once at install.
std::uintptr_t g_tickCall     = 0;
std::uintptr_t g_tickTarget   = 0;
std::uintptr_t g_slowCall     = 0;
std::uintptr_t g_slowTarget   = 0;
std::uintptr_t g_slot         = 0;  // EndUISystem[19]
void**         g_gameMode     = nullptr;

platform::CallRedirect g_tick;  // placed on the game thread (the call crosses a block)
platform::CallRedirect g_slow;

/// A seat's pad's key events: routed into the seat's context, or not (its menu not up, its path empty, its scope
/// refused).
struct Routing
{
    std::uint32_t routed = 0, notReady = 0, noPath = 0, notHeld = 0;
    bool          lastRouted = true;  // a change is logged at Debug
};

// Game thread. Index = seat; [0] = player 1 (the game's own tick, counted for the reading; its routing and buffer
// flag unused).
struct Seat
{
    std::uint32_t ticks        = 0;  // the controller ticked at the game's spot
    std::uint32_t slowWrites   = 0;  // its tick wrote the world-slow slot
    bool          bufferLogged = false;
    Routing       routing;
};
std::array<Seat, players::scopes::kMaxPlayers> g_seats;
bool                                     g_enabled     = true;
bool                                     g_placed      = false;
bool                                     g_ticking     = false;
int                                      g_tickingSeat = 0;
int                                      g_eventSeat   = 0;
bool                                     g_aloneLogged = false;
command_menu_rules::SlowWrites           g_slowWrites;
std::optional<players::scopes::Scope>    g_eventScope;  // the seat's context around one of its pad's key events

void LogMenuSlow(const wchar_t* why) noexcept;  // the slow-down's state, at Info (below)

std::uint8_t* Ctrl1() noexcept
{
    std::uint8_t* ctrl = nullptr;
    return Read(reinterpret_cast<const void*>(g_slot), ctrl) ? ctrl : nullptr;
}

/// The game's leader-only input buffer, run for the seat's member under the game's gates (the battle system
/// exists, the game state's pause counter is 0).
void RunInputBuffer(int seat, float deltaTime) noexcept
{
    void*         system    = players::field_root::BattleSystem();
    void*         gameMode  = nullptr;
    std::uint8_t* state     = nullptr;
    std::int32_t  paused    = 0;
    void*         member    = players::scopes::Member(seat);
    std::uint8_t* component = nullptr;
    void**        vtable    = nullptr;
    InputBufferFn buffer    = nullptr;
    if (system == nullptr || !Read(g_gameMode, gameMode) || gameMode == nullptr ||
        !Read(static_cast<std::uint8_t*>(gameMode) + facts::kGameModeGameState, state) || state == nullptr ||
        !Read(state + facts::kGameStatePauseCounter, paused) || paused != 0 || member == nullptr ||
        !Read(static_cast<std::uint8_t*>(member) + facts::kCharacterBattleComponent, component) || component == nullptr ||
        !Read(component, vtable) || !Read(reinterpret_cast<std::uint8_t*>(vtable) + facts::kBattleCompInputBufferSlot, buffer))
    {
        return;
    }
    Seat& s = g_seats[seat];
    if (!s.bufferLogged)
    {
        s.bufferLogged = true;
        platform::LogDebug(L"command menu: the game's input buffer runs for {}'s member after its tick", SeatName(seat));
    }
    if (!platform::CallGuarded(buffer, component, deltaTime))
    {
        platform::Log(L"command menu: the input buffer for {}'s member RAISED AN EXCEPTION", SeatName(seat));
    }
}

/// The seat's controller ticked inside the seat's scopes, with its pad's buttons and sticks and its target fields,
/// then the game's input buffer for its member.
void TickSeat(int seat, float deltaTime)
{
    void* ctrl = command_menu::Controller(seat);
    if (ctrl == nullptr || !command_menu::Ready(seat) || players::scopes::Member(seat) == nullptr)
    {
        return;
    }
    players::scopes::Scope                 scope(seat, players::scopes::All);
    players::pad_buttons::ButtonScope      buttons(static_cast<unsigned>(seat));
    players::seat_camera::StickScope stick(static_cast<std::uint32_t>(seat), true);
    // A command executed here aims at the displayed target and re-locks the camera on it while the member is the
    // leader and locked on: the seat's target fields, not player 1's (RE-lock-camera-ledger.md row X4).
    const targeting::FieldsScope targets(seat);
    if (!scope.Held())
    {
        return;
    }
    ++g_seats[seat].ticks;
    g_ticking     = true;
    g_tickingSeat = seat;
    reinterpret_cast<TickFn>(g_tickTarget)(ctrl, deltaTime);
    RunInputBuffer(seat, deltaTime);
    if (buttons.Held())
    {
        arena_warp::ReadPress(seat);  // its own pad's Dash (L3): a double click asks for the warp (never player 1's pad)
    }
    g_ticking     = false;
    g_tickingSeat = 0;
}

/// FieldCommandCtrl__Tick at the game's call: the game's controller, then each built seat's in the seat's scopes.
void TickControllers(void* controller, float deltaTime)
{
    ++g_seats[0].ticks;
    g_ticking = true;
    reinterpret_cast<TickFn>(g_tickTarget)(controller, deltaTime);
    g_ticking = false;
    if (!g_enabled || controller != Ctrl1())
    {
        return;
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        TickSeat(seat, deltaTime);
    }
}

/// GameState_SetGameSpeedSlot at the tick's one write: a seat's tick writes the least of the frame's values
/// (battle/command_menu_rules.h). The site lies inside the controller tick; a write outside one counts as the game's.
std::int64_t SetSlot(std::uint8_t slot, float value)
{
    const int seat = g_ticking ? g_tickingSeat : 0;
    ++g_seats[seat].slowWrites;
    const std::uint32_t aloneBefore = g_slowWrites.Alone();
    value                           = g_slowWrites.Write(seat, value);
    if (seat != 0 && g_slowWrites.Alone() != aloneBefore && !g_aloneLogged)
    {
        g_aloneLogged = true;  // the game's tick writes every frame (RE-battle.md §2.7): this line means that changed
        platform::Log(L"command menu: WARNING: {}'s tick wrote the world slow with no write of player 1's this frame; "
                      L"written as asked",
                      SeatName(seat));
    }
    return reinterpret_cast<SetSlotFn>(g_slowTarget)(slot, value);
}

void NoteRouting(int seat, bool routed, const wchar_t* why) noexcept
{
    Routing& routing = g_seats[seat].routing;
    if (routed != routing.lastRouted)
    {
        routing.lastRouted = routed;
        platform::LogDebug(L"command menu: {}'s pad's key events {} its context ({})", SeatName(seat),
                           routed ? L"reach" : L"do NOT reach", why);
    }
}

/// A seat's pad's key events reach the seat's widgets inside the seat's context, once its menu is up in that
/// context. The pad's XInput user is the seat.
bool EnterEventContext(unsigned user) noexcept
{
    const int seat = static_cast<int>(user);
    if (!g_enabled || !SeatBuilt(seat) || g_eventScope.has_value())
    {
        return false;
    }
    Routing& routing = g_seats[seat].routing;
    if (!command_menu::Ready(seat))
    {
        ++routing.notReady;
        NoteRouting(seat, false, L"its menu is not set up");
        return false;
    }
    if (!players::scopes::FocusPathSet(seat))
    {
        ++routing.noPath;
        NoteRouting(seat, false, L"its focus path is empty");
        return false;
    }
    g_eventScope.emplace(seat, players::scopes::Focus | players::scopes::UiSlot);
    if (!g_eventScope->Held())
    {
        g_eventScope.reset();
        ++routing.notHeld;
        NoteRouting(seat, false, L"its focus scope could not open");
        return false;
    }
    g_eventSeat = seat;
    ++routing.routed;
    NoteRouting(seat, true, L"its menu is up in its context");
    return true;
}

void LeaveEventContext(unsigned user) noexcept
{
    if (static_cast<int>(user) != g_eventSeat)
    {
        return;
    }
    g_eventScope.reset();
    g_eventSeat = 0;
}

/// Player 1's and each seat's tick and slow-write counts, the seats' key-event routing, the alone writes.
std::wstring Reading()
{
    std::wstring text = std::format(L"player 1: ticks {}, slow writes {}", g_seats[0].ticks, g_seats[0].slowWrites);
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        text += std::format(L"; {}: ticks {}, slow writes {}, pad events routed {}, refused (not ready {}, no path {}, not held {})",
                            SeatName(seat), s.ticks, s.slowWrites, s.routing.routed, s.routing.notReady, s.routing.noPath,
                            s.routing.notHeld);
    }
    return text + std::format(L"; seat writes without player 1's {}; an open menu {}", g_slowWrites.Alone(),
                              g_slowWrites.Slows() ? L"slows the world" : L"does not slow the world (switched off)");
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tickCall = image.Rebase(facts::kCommandCtrlTickCall, 5);
    const auto tick     = platform::FindFunction(image, facts::kCommandCtrlTick);
    const auto slowCall = image.Rebase(facts::kCommandCtrlSlowCall, 5);
    const auto slow     = platform::FindFunction(image, facts::kSetGameSpeedSlot);
    const auto slot     = image.Rebase(facts::kCommandCtrlSlotGlobal, sizeof(void*));
    const auto gameMode = image.Rebase(facts::kGameModeGlobal, sizeof(void*));
    if (!tickCall || !tick || !slowCall || !slow || !slot || !players::field_root::Installed() || !gameMode)
    {
        platform::Log(L"command menu: the tick's engine checks failed; no seat's menu ever ticks, and the menu-slow "
                      L"switch does nothing");
        return false;
    }
    g_tickCall     = *tickCall;
    g_tickTarget   = *tick;
    g_slowCall     = *slowCall;
    g_slowTarget   = *slow;
    g_slot         = *slot;
    g_gameMode     = reinterpret_cast<void**>(*gameMode);
    // The menu-slow setting (one of the table's words), set before the redirect is placed: no write reads it yet.
    g_slowWrites.SetSlows(platform::settings::Of(platform::settings::kMenuSlow).word == L"on");
    if (!g_slow.Install(g_slowCall, g_slowTarget, reinterpret_cast<std::uintptr_t>(&SetSlot)))
    {
        platform::Log(L"command menu: the time-slow write could not be redirected; no seat's menu ever ticks, and the "
                      L"menu-slow switch does nothing");
        return false;
    }
    LogMenuSlow(platform::settings::Why(platform::settings::kMenuSlow).c_str());
    players::pad_buttons::SetUiContext({&EnterEventContext, &LeaveEventContext});
    control::RegisterOption(
        platform::settings::kMenuSlow,
        [](const std::wstring& word) {
            SetMenuSlow(word == L"on", L"the console");
            return control::Outcome{};
        },
        [] { return std::wstring(MenuSlow() ? L"on" : L"off"); });
    control::Register({.name = L"command-menu.route", .shape = control::Shape::Text,
                       .help = L"each command controller's ticks at the game's spot and its world-slow writes; each seat's "
                               L"pad key events routed into the seat's context and those refused (menu not set up, path "
                               L"empty, scope not held); seat writes made with no write of player 1's in the frame",
                       .get  = [] { return control::Value{Reading()}; }});
    return true;
}

void Remove() noexcept
{
    players::pad_buttons::SetUiContext({});
    g_tick.Restore();
    g_slow.Restore();
}

void Tick() noexcept
{
    g_slowWrites.BeginFrame();
    if (g_placed || g_tickCall == 0)
    {
        return;
    }
    g_placed = true;  // once, whatever the outcome
    if (g_tick.InstallFromOwnThread(g_tickCall, g_tickTarget, reinterpret_cast<std::uintptr_t>(&TickControllers)))
    {
        platform::Log(L"command menu: the controller tick's call is redirected; each seat's controller ticks after the game's");
    }
    else
    {
        platform::Log(L"command menu: the controller tick's call could not be redirected; no seat's menu ever ticks");
    }
}

bool Ticking() noexcept
{
    return g_ticking;
}

int TickingSeat() noexcept
{
    return g_tickingSeat;
}

int EventSeat() noexcept
{
    return g_eventSeat;
}

bool MenuSlow() noexcept
{
    return g_slowWrites.Slows();
}

void SetMenuSlow(bool slows, const wchar_t* why) noexcept
{
    if (!g_slow.IsInstalled())
    {
        platform::LogWarning(L"battle menus: the menu's world-slow write is not redirected (see the install lines); {} "
                             L"cannot switch the slow-down",
                             why);
        return;
    }
    g_slowWrites.SetSlows(slows);
    LogMenuSlow(why);  // a waiting menu's grey follows at the grey's tick, later in the same engine tick (battle/menu_grey)
}

namespace {

void LogMenuSlow(const wchar_t* why) noexcept
{
    if (g_slowWrites.Slows())
    {
        platform::Log(L"battle menus: an open command menu slows the world, as the game does ({})", why);
    }
    else
    {
        platform::Log(L"battle menus: an open command menu no longer slows the world or greys the screen; the world and the "
                      L"ATB gauges run on at full speed ({})",
                      why);
    }
}

}  // namespace

}  // namespace lss::battle::command_menu_tick
