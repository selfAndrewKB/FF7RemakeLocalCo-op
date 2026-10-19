#include "battle/command_menu_tick.h"

#include "battle/command_menu_two.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/pad_buttons.h"
#include "players/player_two_camera.h"
#include "players/scopes.h"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace lss::battle::command_menu_tick {

namespace {

using TickFn        = void (*)(void* controller, float deltaTime);
using SetSlotFn     = std::int64_t (*)(std::uint8_t slot, float value);
using InputBufferFn = void (*)(void* component, float deltaTime);

constexpr int kPlayer = 1;

// Set once at install.
std::uintptr_t g_tickCall    = 0;
std::uintptr_t g_tickTarget  = 0;
std::uintptr_t g_slowCall    = 0;
std::uintptr_t g_slowTarget  = 0;
std::uintptr_t g_slot        = 0;  // EndUISystem[19]
void**         g_battleSystem = nullptr;
void**         g_gameMode    = nullptr;

platform::CallRedirect g_tick;  // placed on the game thread (the call crosses a block)
platform::CallRedirect g_slow;

// Game thread.
int   g_ticking   = 0;
float g_slowOne   = 1.0f;   // player 1's slot-2 value this frame
bool  g_enabled   = true;
bool  g_placed    = false;
bool  g_bufferLogged = false;
std::uint32_t g_ticksTwo = 0;
std::optional<players::scopes::Scope> g_eventScope;  // player 2's context around one of pad 2's key events

template <class T>
bool Read(const void* address, T& out) noexcept
{
    return address != nullptr && platform::CopyGuarded(&out, address, sizeof(T));
}

std::uint8_t* Ctrl1() noexcept
{
    std::uint8_t* ctrl = nullptr;
    return Read(reinterpret_cast<const void*>(g_slot), ctrl) ? ctrl : nullptr;
}

/// The game's leader-only input buffer, run for player 2's member under the game's gates (the battle system
/// exists, the game state's pause counter is 0).
void RunInputBuffer(float deltaTime) noexcept
{
    void*         system   = nullptr;
    void*         gameMode = nullptr;
    std::uint8_t* state    = nullptr;
    std::int32_t  paused   = 0;
    void*         member   = players::scopes::Member(kPlayer);
    std::uint8_t* component = nullptr;
    void**        vtable   = nullptr;
    InputBufferFn buffer   = nullptr;
    if (!Read(g_battleSystem, system) || system == nullptr || !Read(g_gameMode, gameMode) || gameMode == nullptr ||
        !Read(static_cast<std::uint8_t*>(gameMode) + facts::kGameModeGameState, state) || state == nullptr ||
        !Read(state + facts::kGameStatePauseCounter, paused) || paused != 0 || member == nullptr ||
        !Read(static_cast<std::uint8_t*>(member) + facts::kCharacterBattleComponent, component) || component == nullptr ||
        !Read(component, vtable) || !Read(reinterpret_cast<std::uint8_t*>(vtable) + facts::kBattleCompInputBufferSlot, buffer))
    {
        return;
    }
    if (!g_bufferLogged)
    {
        g_bufferLogged = true;
        platform::LogDebug(L"command menu 2: the game's input buffer runs for player 2's member after its tick");
    }
    if (!platform::CallGuarded(buffer, component, deltaTime))
    {
        platform::Log(L"command menu 2: the input buffer for player 2's member RAISED AN EXCEPTION");
    }
}

/// FieldCommandCtrl__Tick at the game's call: the game's controller, then player 2's in player 2's scopes.
void TickControllers(void* controller, float deltaTime)
{
    g_ticking = 1;
    reinterpret_cast<TickFn>(g_tickTarget)(controller, deltaTime);
    g_ticking = 0;
    void* two = command_menu_two::Controller();
    if (!g_enabled || two == nullptr || controller != Ctrl1() || !command_menu_two::Ready() ||
        players::scopes::Member(kPlayer) == nullptr)
    {
        return;
    }
    players::scopes::Scope             scope(kPlayer, players::scopes::All);
    players::pad_buttons::ButtonScope  buttons(1);
    players::player_two_camera::StickScope stick(1, true);
    if (!scope.Held())
    {
        return;
    }
    ++g_ticksTwo;
    g_ticking = 2;
    reinterpret_cast<TickFn>(g_tickTarget)(two, deltaTime);
    RunInputBuffer(deltaTime);
    g_ticking = 0;
}

/// GameState_SetGameSpeedSlot at the tick's one write: player 2's tick writes the smaller of both menus' values.
std::int64_t SetSlot(std::uint8_t slot, float value)
{
    if (g_ticking == 2)
    {
        value = std::min(value, g_slowOne);
    }
    else
    {
        g_slowOne = value;
    }
    return reinterpret_cast<SetSlotFn>(g_slowTarget)(slot, value);
}

/// Pad 2's key events reach player 2's widgets inside player 2's context, once its menu is up and no modal layer
/// sits above player 1's own menu.
bool EnterEventContext(unsigned user) noexcept
{
    if (user != 1 || !g_enabled || g_eventScope.has_value() || !command_menu_two::Ready() ||
        !players::scopes::FocusPathSet(kPlayer) || !players::scopes::TopLayerIsCommandMenu())
    {
        return false;
    }
    g_eventScope.emplace(kPlayer, players::scopes::Focus | players::scopes::UiSlot);
    if (!g_eventScope->Held())
    {
        g_eventScope.reset();
        return false;
    }
    return true;
}

void LeaveEventContext(unsigned) noexcept
{
    g_eventScope.reset();
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tickCall = image.Rebase(facts::kCommandCtrlTickCall, 5);
    const auto tick     = platform::FindFunction(image, facts::kCommandCtrlTick);
    const auto slowCall = image.Rebase(facts::kCommandCtrlSlowCall, 5);
    const auto slow     = platform::FindFunction(image, facts::kSetGameSpeedSlot);
    const auto slot     = image.Rebase(facts::kCommandCtrlSlotGlobal, sizeof(void*));
    const auto system   = image.Rebase(facts::kBattleSystemGlobal, sizeof(void*));
    const auto gameMode = image.Rebase(facts::kGameModeGlobal, sizeof(void*));
    if (!tickCall || !tick || !slowCall || !slow || !slot || !system || !gameMode)
    {
        platform::Log(L"command menu 2: the tick's engine checks failed; player 2's menu never ticks");
        return false;
    }
    g_tickCall     = *tickCall;
    g_tickTarget   = *tick;
    g_slowCall     = *slowCall;
    g_slowTarget   = *slow;
    g_slot         = *slot;
    g_battleSystem = reinterpret_cast<void**>(*system);
    g_gameMode     = reinterpret_cast<void**>(*gameMode);
    if (!g_slow.Install(g_slowCall, g_slowTarget, reinterpret_cast<std::uintptr_t>(&SetSlot)))
    {
        platform::Log(L"command menu 2: the time-slow write could not be redirected; player 2's menu never ticks");
        return false;
    }
    players::pad_buttons::SetUiContext({&EnterEventContext, &LeaveEventContext});
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
    if (g_placed || g_tickCall == 0)
    {
        return;
    }
    g_placed = true;  // once, whatever the outcome
    if (g_tick.InstallFromOwnThread(g_tickCall, g_tickTarget, reinterpret_cast<std::uintptr_t>(&TickControllers)))
    {
        platform::Log(L"command menu 2: the controller tick's call is redirected; player 2's controller ticks after the game's");
    }
    else
    {
        platform::Log(L"command menu 2: the controller tick's call could not be redirected; player 2's menu never ticks");
    }
}

int TickingPlayer() noexcept
{
    return g_ticking;
}

}  // namespace lss::battle::command_menu_tick
