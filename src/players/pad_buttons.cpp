#include "players/pad_buttons.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/action_tables.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::players::pad_buttons {

namespace {

using TickFn     = void (*)(void* self, float deltaTime, void* slateApp, void* cursor);
using KeyEventFn = std::int64_t (*)(void* self, void* slateApp, const std::uint8_t* keyEvent);
using DispatchFn = char (*)(void* owner, void* unused, void* actionEvent, std::uint8_t alreadyHandled);
using RouteUpFn  = std::int64_t (*)(void* owner, void* actionEvent);

constexpr unsigned kMaxPads = 4;  // XInput users 0..3
constexpr action_tables::Layout kLayout{.raw = facts::kActionTableRaw, .ui = facts::kActionTableUi, .dpad = facts::kDpadBits};

// Set once at install.
std::uint8_t**         g_owner = nullptr;  // the global holding the input object
platform::VTableEntry  g_tickSlot, g_keyDownSlot, g_keyUpSlot;
platform::EntryRedirect g_dispatch;
platform::CallRedirect  g_routeUp;
TickFn                 g_tick     = nullptr;
KeyEventFn             g_keyDown  = nullptr;
KeyEventFn             g_keyUp    = nullptr;

// Game thread only.
int                                           g_players = 1;
std::array<action_tables::View, kMaxPads>     g_views;  // index = XInput user; 0 unused (the live tables)
UiContext                                     g_ui;
bool                                          g_enabled   = true;
bool                                          g_inScope   = false;
unsigned                                      g_eventUser = 0;      // the pad whose key event is being handled
bool                                          g_eventRouted = false;  // ... inside its player's UI context
std::uint32_t                                 g_events[kMaxPads]{};
DWORD                                         g_thread = 0;
bool                                          g_offThreadLogged = false;
bool                                          g_buildFailedLogged = false;

std::uint8_t* Owner() noexcept
{
    std::uint8_t* owner = nullptr;
    return g_owner != nullptr && platform::CopyGuarded(&owner, g_owner, sizeof(owner)) ? owner : nullptr;
}

bool OwnPad(unsigned user) noexcept
{
    return g_enabled && user >= 1 && user < static_cast<unsigned>(g_players) && user < kMaxPads;
}

bool OnGameThread() noexcept
{
    const DWORD now = GetCurrentThreadId();
    if (now != g_thread && !g_offThreadLogged)
    {
        g_offThreadLogged = true;
        platform::LogWarning(L"pad buttons: the input object was driven off the game thread; that call is passed through");
    }
    return now == g_thread;
}

/// The pad's copy, built on first use and rebuilt if the tables changed shape. Null if it cannot be.
action_tables::View* ViewFor(unsigned user, std::uint8_t* owner) noexcept
{
    action_tables::View& view = g_views[user];
    if (action_tables::Fits(owner, kLayout, view))
    {
        return &view;
    }
    const bool rebuilt = view.built;
    if (!action_tables::Build(owner, kLayout, view))
    {
        if (!g_buildFailedLogged)
        {
            g_buildFailedLogged = true;
            platform::LogWarning(L"pad buttons: pad {}'s copy could not be built (an action table is empty); its presses stay player 1's", user + 1);
        }
        return nullptr;
    }
    platform::LogDebug(L"pad buttons: pad {}'s copy {} ({} + {} actions)", user + 1, rebuilt ? L"rebuilt (the tables changed shape)" : L"built",
                       view.rawShape.names.size(), view.uiShape.names.size());
    return &view;
}

/// Exchanges a pad's copy with the live bytes; true if it did (the caller exchanges back).
bool Swap(unsigned user) noexcept
{
    std::uint8_t* owner = Owner();
    if (owner == nullptr || g_inScope)
    {
        return false;
    }
    action_tables::View* view = ViewFor(user, owner);
    if (view == nullptr)
    {
        return false;
    }
    action_tables::Exchange(owner, kLayout, *view);
    g_inScope = true;
    return true;
}

void SwapBack(unsigned user) noexcept
{
    std::uint8_t* owner = Owner();
    if (owner != nullptr)
    {
        action_tables::Exchange(owner, kLayout, g_views[user]);
    }
    g_inScope = false;
}

/// One pad's key event handled in its own state and, when its player has one, its own UI context.
template <class Fn, class... Args>
std::int64_t HandleEvent(Fn original, void* self, void* app, const std::uint8_t* event, Args... args) noexcept
{
    std::uint32_t user = 0;
    std::memcpy(&user, event + facts::kKeyEventUserIndex, sizeof(user));
    if (!OwnPad(user) || !OnGameThread() || !Swap(user))
    {
        return original(self, app, event, args...);
    }
    ++g_events[user];
    g_eventUser   = user;
    g_eventRouted = g_ui.enter != nullptr && g_ui.enter(user);
    const std::int64_t result = original(self, app, event, args...);
    if (g_eventRouted && g_ui.leave != nullptr)
    {
        g_ui.leave(user);
    }
    g_eventRouted = false;
    g_eventUser   = 0;
    SwapBack(user);
    return result;
}

std::int64_t KeyDown(void* self, void* app, const std::uint8_t* event)
{
    return HandleEvent(g_keyDown, self, app, event);
}

std::int64_t KeyUp(void* self, void* app, const std::uint8_t* event)
{
    return HandleEvent(g_keyUp, self, app, event);
}

/// The dispatcher: a further pad's press outside its player's UI context is "already handled", the game's own
/// argument for a press the UI must not see (the raw table is still written: the pad's copy).
char Dispatch(void* owner, void* unused, void* actionEvent, std::uint8_t alreadyHandled)
{
    if (g_eventUser != 0 && !g_eventRouted)
    {
        alreadyHandled = 1;
    }
    return reinterpret_cast<DispatchFn>(g_dispatch.Original())(owner, unused, actionEvent, alreadyHandled);
}

/// The release's route: skipped for a further pad's release outside its player's UI context (it would reach
/// player 1's capturer and focus path).
std::int64_t RouteUp(void* owner, void* actionEvent)
{
    if (g_eventUser != 0 && !g_eventRouted)
    {
        return 0;
    }
    return reinterpret_cast<RouteUpFn>(g_routeUp.Original())(owner, actionEvent);
}

/// The input tick: the stick aggregation sees pad 1's record alone, then each pad's copy gets the game's advance.
void Tick(void* self, float deltaTime, void* slateApp, void* cursor)
{
    std::uint8_t* owner = static_cast<std::uint8_t*>(self);
    if (g_players <= 1 || !g_enabled || !OnGameThread())
    {
        g_tick(self, deltaTime, slateApp, cursor);
        return;
    }
    std::uint8_t* bank  = owner + facts::kStickBanks + 16 * facts::kXInputBank + facts::kStickBankCount;
    std::int32_t  count = 0;
    std::memcpy(&count, bank, sizeof(count));
    const std::int32_t one = 1;
    if (count > 1)
    {
        std::memcpy(bank, &one, sizeof(one));
    }
    g_tick(self, deltaTime, slateApp, cursor);
    if (count > 1)
    {
        std::memcpy(bank, &count, sizeof(count));
    }
    for (unsigned user = 1; user < kMaxPads; ++user)
    {
        if (OwnPad(user) && g_views[user].built)
        {
            action_tables::Advance(g_views[user]);
        }
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_tickSlot.Active())
    {
        return false;
    }
    const auto owner    = image.Rebase(facts::kSquareInputOwnerGlobal, sizeof(void*));
    const auto dispatch = platform::FindFunction(image, facts::kActionDispatch);
    const auto routeUp  = platform::FindFunction(image, facts::kRouteActionUp);
    const auto routeUpCall = image.Rebase(facts::kRouteActionUpCall, 5);
    if (!owner || !dispatch || !routeUp || !routeUpCall)
    {
        platform::Log(L"pad buttons: an engine check failed; every pad presses player 1's buttons");
        return false;
    }
    g_owner = reinterpret_cast<std::uint8_t**>(*owner);

    // All or none: a pad's copy without the dispatcher's flag would still open player 1's menus.
    const std::span<const std::uint8_t> pushes(facts::kActionDispatchEntry.data(), facts::kActionDispatchPushes);
    const bool installed =
        g_dispatch.Install(*dispatch, pushes, reinterpret_cast<std::uintptr_t>(&Dispatch)) &&
        g_routeUp.Install(*routeUpCall, *routeUp, reinterpret_cast<std::uintptr_t>(&RouteUp)) &&
        platform::ReplaceSlot(image, facts::kInputTick, reinterpret_cast<std::uintptr_t>(&Tick), g_tickSlot) &&
        platform::ReplaceSlot(image, facts::kInputKeyDown, reinterpret_cast<std::uintptr_t>(&KeyDown), g_keyDownSlot) &&
        platform::ReplaceSlot(image, facts::kInputKeyUp, reinterpret_cast<std::uintptr_t>(&KeyUp), g_keyUpSlot);
    if (!installed)
    {
        Remove();
        platform::Log(L"pad buttons: a redirect or slot could not be placed; every pad presses player 1's buttons");
        return false;
    }
    // The slots' own functions, as the slots held them (UE4SS keeps a detour at the key-down handler's entry; the
    // slot replacement calls through it, as the engine did).
    g_tick    = reinterpret_cast<TickFn>(g_tickSlot.Original());
    g_keyDown = reinterpret_cast<KeyEventFn>(g_keyDownSlot.Original());
    g_keyUp   = reinterpret_cast<KeyEventFn>(g_keyUpSlot.Original());
    control::Register({.name  = L"pad-buttons",
                       .shape = control::Shape::OnOff,
                       .help  = L"each further pad's buttons are its own player's (players/pad_buttons.h; on)",
                       .set   = [](const control::Value& value) {
                           g_enabled = std::get<bool>(value);
                           platform::Log(L"pad buttons {}", g_enabled ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"pad.events", .shape = control::Shape::Integer, .max = INT32_MAX,
                       .help = L"key events handled in pad 2's own state so far",
                       .get  = [] { return control::Value{static_cast<std::int64_t>(g_events[1])}; }});
    platform::Log(L"pad buttons: installed; each further pad's buttons are its own player's");
    return true;
}

void Remove() noexcept
{
    g_keyUpSlot.Restore();
    g_keyDownSlot.Restore();
    g_tickSlot.Restore();
    g_routeUp.Restore();
    g_dispatch.Restore();
}

void SetPlayers(int players) noexcept
{
    if (players == g_players)
    {
        return;
    }
    g_players = players;
    g_thread  = GetCurrentThreadId();  // the game thread: joins and leaves run on it, as the input object does
    for (auto& view : g_views)
    {
        view = action_tables::View{};  // a pad's copy starts from nothing held
    }
    platform::LogDebug(L"pad buttons: {} player(s); pads 2..{} have their own buttons", players, players);
}

ButtonScope::ButtonScope(unsigned user) noexcept
{
    if (OwnPad(user) && OnGameThread() && Swap(user))
    {
        user_ = user;
    }
}

ButtonScope::~ButtonScope()
{
    if (user_ != 0)
    {
        SwapBack(user_);
    }
}

void SetUiContext(UiContext context) noexcept
{
    g_ui = context;
}

}  // namespace lss::players::pad_buttons
