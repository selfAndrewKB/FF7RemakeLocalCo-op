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
#include "players/game_state.h"
#include "players/menu_pads.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <vector>

#include <Unreal/NameTypes.hpp>

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
std::uint32_t                                 g_pads = 0;  // bit u: pad u has a player of its own (bit 0 never set)
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
// The menus' share of the pads (players/menu_pads.h). Game thread only.
bool                                          g_menus       = true;   // the switch `pad-menus`
bool                                          g_menuTime    = false;  // as the last input tick saw it (for the log)
std::array<std::uintptr_t, 3>                 g_openerAt{};           // the PauseMenu, MapMenu, EnemyIntel FKey globals
std::array<std::uint64_t, 3>                  g_openers{};            // their FNames (all 0 until all three read)
std::array<menu_pads::Keys, kMaxPads>         g_keys;                 // each further pad's keys down, by route
std::vector<action_tables::State>             g_before;               // the copy's raw states before a new press
std::vector<std::uint64_t>                    g_pressed;              // what that press pressed in the copy
std::uint32_t                                 g_toGame[kMaxPads]{};   // key events handled as the game's own pad's
std::uint32_t                                 g_opens       = 0;      // opening presses also given to the game
bool                                          g_mixedLogged = false;

std::uint8_t* Owner() noexcept
{
    std::uint8_t* owner = nullptr;
    return g_owner != nullptr && platform::CopyGuarded(&owner, g_owner, sizeof(owner)) ? owner : nullptr;
}

bool OwnPad(unsigned user) noexcept
{
    return g_enabled && user >= 1 && user < kMaxPads && ((g_pads >> user) & 1u) != 0;
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

void NameText(std::int64_t bits, std::wstring* out) noexcept
try
{
    *out = RC::Unreal::FName(bits).ToString();
}
catch (...)
{
    out->clear();
}

std::wstring Names(std::span<const std::uint64_t> names) noexcept
try
{
    std::wstring text;
    for (const std::uint64_t name : names)
    {
        std::wstring one;
        platform::CallGuarded(&NameText, static_cast<std::int64_t>(name), &one);
        text += std::format(L"{}{}", text.empty() ? L"" : L", ", one.empty() ? std::format(L"{:#x}", name) : one);
    }
    return text;
}
catch (...)
{
    return {};
}

/// The openers' names from the game's FKey globals; all three or none (a partial read could replay a press that
/// pressed an unread opener's neighbour). True once read.
bool ReadOpeners() noexcept
{
    std::array<std::uint64_t, 3> names{};
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (g_openerAt[i] == 0 ||
            !platform::CopyGuarded(&names[i], reinterpret_cast<const void*>(g_openerAt[i]), sizeof(names[i])) ||
            names[i] == 0)
        {
            return false;
        }
    }
    g_openers = names;
    return true;
}

bool MenuTime() noexcept
{
    return g_menus && game_state::MenuOwnsInput();
}

struct Handled
{
    std::int64_t result = 0;
    bool         inCopy = false;  // false: the copy could not be exchanged in and the game's own state took it
};

/// One pad's key event handled in its own copy and, when `ui` and its player has one, its own UI context. With
/// `snapshot`, the copy's raw states before the event are kept in g_before.
Handled InCopy(KeyEventFn original, void* self, void* app, const std::uint8_t* event, unsigned user, bool ui,
               bool snapshot) noexcept
{
    std::uint8_t*        owner = Owner();
    action_tables::View* view  = owner != nullptr && !g_inScope ? ViewFor(user, owner) : nullptr;
    if (view == nullptr)
    {
        return {original(self, app, event), false};
    }
    if (snapshot)
    {
        try
        {
            g_before.assign(view->raw.begin(), view->raw.end());
        }
        catch (...)
        {
            g_before.clear();  // no snapshot: Pressed finds nothing, the press stays the player's own
        }
    }
    action_tables::Exchange(owner, kLayout, *view);
    g_inScope = true;
    ++g_events[user];
    g_eventUser   = user;
    g_eventRouted = ui && g_ui.enter != nullptr && g_ui.enter(user);
    const std::int64_t result = original(self, app, event);
    if (g_eventRouted && g_ui.leave != nullptr)
    {
        g_ui.leave(user);
    }
    g_eventRouted = false;
    g_eventUser   = 0;
    SwapBack(user);
    return {result, true};
}

/// One pad's key event as the game's own pad event: its tables, its dispatcher's own argument, its UI route.
std::int64_t ToGame(KeyEventFn original, void* self, void* app, const std::uint8_t* event, unsigned user) noexcept
{
    ++g_toGame[user];
    return original(self, app, event);
}

/// A further pad's key-down: a key already down repeats where its press went; a new press goes to the game while a
/// menu owns input, otherwise to the pad's copy, and also to the game when it pressed only menu openers there.
std::int64_t KeyDown(void* self, void* app, const std::uint8_t* event)
{
    std::uint32_t user = 0;
    std::uint64_t key  = 0;
    std::memcpy(&user, event + facts::kKeyEventUserIndex, sizeof(user));
    std::memcpy(&key, event + facts::kKeyEventKey, sizeof(key));
    if (!OwnPad(user) || !OnGameThread())
    {
        return g_keyDown(self, app, event);
    }
    menu_pads::Keys&   keys     = g_keys[user];
    const bool         menuTime = MenuTime();
    const bool         newPress = menu_pads::Where(keys, key) == 0;
    std::uint8_t       route    = menu_pads::Route(keys, key, menuTime);
    std::int64_t       result   = 0;
    if ((route & menu_pads::kToCopy) != 0)
    {
        const bool check = g_menus && newPress && !menuTime && g_openers[0] != 0;
        const Handled handled = InCopy(g_keyDown, self, app, event, user, !menuTime, check);
        if (!handled.inCopy)
        {
            if (newPress)
            {
                menu_pads::Mark(keys, key, menu_pads::kToGame);  // the game's tables took it: release it there
            }
            return handled.result;
        }
        result = handled.result;
        if (newPress)
        {
            menu_pads::Mark(keys, key, menu_pads::kToCopy);
        }
        if (check)
        {
            try
            {
                menu_pads::Pressed(g_views[user].rawShape, g_before, g_views[user].raw, g_pressed);
            }
            catch (...)
            {
                g_pressed.clear();
            }
            if (menu_pads::OpensMenu(g_pressed, g_openers, key))
            {
                route |= menu_pads::kToGame;
                ++g_opens;
                if (platform::Enabled(platform::Level::Debug))
                {
                    platform::LogDebug(L"pad buttons: pad {}'s press of {} opens the game's menu too", user + 1,
                                       Names(g_pressed));
                }
            }
            else if (!g_mixedLogged && menu_pads::AnyOpener(g_pressed, g_openers))
            {
                g_mixedLogged = true;
                platform::Log(L"pad buttons: pad {}'s press pressed a menu opener with other actions ({}); kept as its "
                              L"player's own, the menu not opened", user + 1, Names(g_pressed));
            }
        }
    }
    if ((route & menu_pads::kToGame) != 0)
    {
        const std::int64_t game = ToGame(g_keyDown, self, app, event, user);
        if ((route & menu_pads::kToCopy) == 0)
        {
            result = game;
        }
        if (newPress)
        {
            menu_pads::Mark(keys, key, menu_pads::kToGame);
        }
    }
    return result;
}

/// A further pad's key-up: released where its press went (a key never recorded: the game's while a menu owns input,
/// otherwise the pad's copy).
std::int64_t KeyUp(void* self, void* app, const std::uint8_t* event)
{
    std::uint32_t user = 0;
    std::uint64_t key  = 0;
    std::memcpy(&user, event + facts::kKeyEventUserIndex, sizeof(user));
    std::memcpy(&key, event + facts::kKeyEventKey, sizeof(key));
    if (!OwnPad(user) || !OnGameThread())
    {
        if (user >= 1 && user < kMaxPads && GetCurrentThreadId() == g_thread)  // no off-thread warning for pad 1
        {
            menu_pads::Forget(g_keys[user], key);
        }
        return g_keyUp(self, app, event);
    }
    menu_pads::Keys&   keys     = g_keys[user];
    const bool         menuTime = MenuTime();
    const std::uint8_t route    = menu_pads::Route(keys, key, menuTime);
    menu_pads::Forget(keys, key);
    std::int64_t result = 0;
    if ((route & menu_pads::kToCopy) != 0)
    {
        // The release keeps the player's UI context even in menu time: its press may have been captured there.
        const Handled handled = InCopy(g_keyUp, self, app, event, user, true, false);
        if (!handled.inCopy)
        {
            return handled.result;  // the game's own state took the release
        }
        result = handled.result;
    }
    if ((route & menu_pads::kToGame) != 0)
    {
        const std::int64_t game = ToGame(g_keyUp, self, app, event, user);
        if ((route & menu_pads::kToCopy) == 0)
        {
            result = game;
        }
    }
    return result;
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

/// The input tick: the stick aggregation sees pad 1's record alone (every pad's while a menu owns input: the game's
/// own aggregate, the furthest-pushed stick wins), then each pad's copy gets the game's advance.
void Tick(void* self, float deltaTime, void* slateApp, void* cursor)
{
    std::uint8_t* owner = static_cast<std::uint8_t*>(self);
    if (g_pads == 0 || !g_enabled || !OnGameThread())
    {
        g_tick(self, deltaTime, slateApp, cursor);
        return;
    }
    if (g_openers[0] == 0 && ReadOpeners())
    {
        platform::Log(L"pad buttons: the menu openers' names read; every pad opens the game's menus");
    }
    const bool menuTime = MenuTime();
    if (menuTime != g_menuTime)
    {
        g_menuTime = menuTime;
        platform::Log(L"pad buttons: {}", menuTime ? L"a menu owns input; every pad drives it"
                                                   : L"no menu owns input; each further pad's buttons are its "
                                                     L"own player's again");
    }
    std::uint8_t* bank  = owner + facts::kStickBanks + 16 * facts::kXInputBank + facts::kStickBankCount;
    std::int32_t  count = 0;
    std::memcpy(&count, bank, sizeof(count));
    const std::int32_t one = 1;
    if (menuTime)
    {
        count = 0;  // the game's own aggregate: nothing clamped, nothing to put back
    }
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
    // The menu openers' names, from the game's FKey globals (filled by its static initialisers); read again on the
    // input tick while unread. Menu time works without them.
    const std::uintptr_t openerGlobals[] = {facts::kPauseMenuKeyGlobal, facts::kMapMenuKeyGlobal,
                                            facts::kEnemyIntelKeyGlobal};
    for (std::size_t i = 0; i < g_openerAt.size(); ++i)
    {
        const auto at  = image.Rebase(openerGlobals[i], sizeof(std::uint64_t));
        g_openerAt[i] = at ? *at : 0;
    }
    if (!ReadOpeners())
    {
        platform::LogWarning(L"pad buttons: the menu openers' names are not readable yet; only player 1's pad "
                             L"opens menus until they are");
    }
    g_before.reserve(1024);
    g_pressed.reserve(64);

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
    control::Register({.name  = L"pad-menus",
                       .shape = control::Shape::OnOff,
                       .help  = L"every pad opens the game's menus and drives an open one (players/menu_pads.h; on)",
                       .set   = [](const control::Value& value) {
                           g_menus = std::get<bool>(value);
                           platform::Log(L"pad menus {}", g_menus ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_menus}; }});
    control::Register({.name = L"pad.to-game", .shape = control::Shape::Integer, .max = INT32_MAX,
                       .help = L"pad 2's key events the game handled as its own pad's (menus)",
                       .get  = [] { return control::Value{static_cast<std::int64_t>(g_toGame[1])}; }});
    control::Register({.name = L"pad.to-game-3", .shape = control::Shape::Integer, .max = INT32_MAX,
                       .help = L"pad 3's key events the game handled as its own pad's (menus)",
                       .get  = [] { return control::Value{static_cast<std::int64_t>(g_toGame[2])}; }});
    control::Register({.name = L"pad.menu-opens", .shape = control::Shape::Integer, .max = INT32_MAX,
                       .help = L"further pads' presses of Start, the map or the enemy intel button given to the game",
                       .get  = [] { return control::Value{static_cast<std::int64_t>(g_opens)}; }});
    control::Register({.name = L"pad.events", .shape = control::Shape::Integer, .max = INT32_MAX,
                       .help = L"key events handled in pad 2's own state so far",
                       .get  = [] { return control::Value{static_cast<std::int64_t>(g_events[1])}; }});
    control::Register({.name = L"pad.events-3", .shape = control::Shape::Integer, .max = INT32_MAX,
                       .help = L"key events handled in pad 3's own state so far",
                       .get  = [] { return control::Value{static_cast<std::int64_t>(g_events[2])}; }});
    platform::Log(L"pad buttons: installed; each further pad's buttons are its own player's{}",
                  g_openers[0] != 0 ? L"; every pad opens the game's menus and drives an open one (pad-menus)"
                                     : L"; every pad drives an open menu (pad-menus)");
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

void SetPads(std::uint32_t pads) noexcept
{
    pads &= ((1u << kMaxPads) - 1u) & ~1u;  // pad 1 is player 1's: the game's own state
    if (pads == g_pads)
    {
        return;
    }
    const std::uint32_t added = pads & ~g_pads;
    const std::uint32_t left  = g_pads & ~pads;
    g_pads   = pads;
    g_thread = GetCurrentThreadId();  // the game thread: joins and leaves run on it, as the input object does
    for (unsigned user = 1; user < kMaxPads; ++user)
    {
        if (((added >> user) & 1u) != 0)
        {
            g_views[user] = action_tables::View{};  // a pad newly in starts from nothing held
            g_keys[user]  = menu_pads::Keys{};
        }
        if (((left >> user) & 1u) != 0)
        {
            g_keys[user] = menu_pads::Keys{};  // its releases now pass straight to the game, as every pad's did
        }
    }
    std::wstring own;
    for (unsigned user = 1; user < kMaxPads; ++user)
    {
        if (((pads >> user) & 1u) != 0)
        {
            own += std::format(L"{}{}", own.empty() ? L"" : L", ", user + 1);
        }
    }
    platform::LogDebug(L"pad buttons: pads with their own buttons: {}", own.empty() ? std::wstring(L"none") : own);
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
