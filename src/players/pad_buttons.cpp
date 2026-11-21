#include "players/pad_buttons.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/action_tables.h"
#include "players/game_globals.h"
#include "players/game_state.h"
#include "players/keyboard_keys.h"
#include "players/keyboard_route_rules.h"
#include "players/menu_pads.h"
#include "players/pad_rules.h"
#include "players/pad_table.h"
#include "views/column_layout.h"
#include "views/whole_screen.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <vector>

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
platform::VTableEntry  g_mouseDownSlot, g_mouseUpSlot, g_wheelSlot;
platform::EntryRedirect g_dispatch;
platform::CallRedirect  g_routeUp;
platform::CallRedirect  g_mouseRepeat;  // the input tick's repeat of a held mouse button
platform::EntryRedirect g_note;         // the game's "last device used" note
TickFn                 g_tick      = nullptr;
KeyEventFn             g_keyDown   = nullptr;
KeyEventFn             g_keyUp     = nullptr;
bool                   g_mouseTaken = false;   // the mouse's handlers (the key handlers' shape) and repeat are the mod's

// Game thread only.
std::uint32_t                                 g_pads = 0;  // bit u: pad u has a player of its own (bit 0 never set)
std::array<action_tables::View, kMaxPads>     g_views;  // index = XInput user; 0 unused (the live tables)
UiContext                                     g_ui;
bool                                          g_enabled   = true;
bool                                          g_inScope   = false;
unsigned                                      g_eventUser = 0;      // the pad whose key event is being handled
bool                                          g_eventRouted = false;  // ... inside its player's UI context
std::uint32_t                                 g_events[kMaxPads]{};
platform::GameThreadMark g_thread;
bool                                          g_offThreadLogged = false;
bool                                          g_buildFailedLogged = false;
// The menus', minigames' and bike chases' share of the pads (players/menu_pads.h). Game thread only.
bool                                          g_menus       = true;   // the switch `pad-menus`
bool                                          g_minigames   = true;   // the switch `pad-minigames`
bool                                          g_shared      = false;  // the game's input is shared, as the last input tick saw it
std::uint32_t                                 g_toGameAtStart[kMaxPads]{};  // g_toGame when the shared moment began (its end's report)
std::array<std::uintptr_t, 3>                 g_openerAt{};           // the PauseMenu, MapMenu, EnemyIntel FKey globals
std::array<std::uint64_t, 3>                  g_openers{};            // their FNames (all 0 until all three read)
std::array<menu_pads::Keys, kMaxPads>         g_keys;                 // each further pad's keys down, by route
std::vector<action_tables::State>             g_before;               // the copy's raw states before a new press
std::vector<std::uint64_t>                    g_pressed;              // what that press pressed in the copy
std::uint32_t                                 g_toGame[kMaxPads]{};   // key events handled as the game's own pad's
std::uint32_t                                 g_opens       = 0;      // opening presses also given to the game
bool                                          g_mixedLogged = false;
// The keyboard and mouse as a player's device. Game thread only.
using DownKeys = std::array<keyboard_route_rules::DownKey, 32>;
DownKeys                                      g_keyboardDown{};  // each key down, with the player its press went to
DownKeys                                      g_mouseHeld{};     // each mouse button down, likewise (its repeat goes there too)
void*                                         g_tapApp       = nullptr;  // the Slate application a kept Tab release goes to
std::uint32_t                                 g_keyboardEvents = 0;      // keyboard and mouse events handled in a further player's state
std::uint32_t                                 g_flagsKept    = 0;        // "keyboard used" put back after the keyboard player's event
std::uint32_t                                 g_wheelsDropped = 0;       // wheel turns of the keyboard's player with no UI of theirs open
std::uint32_t                                 g_notesKept    = 0;        // players 2 and 3's pads kept out of the device note
int                                           g_routedSaid   = 0;        // the player the keyboard's events last went to (its change is said)
// The keyboard's digital stick as the last input tick built it. Game thread only.
std::array<float, 4>                          g_keyboardStick{};         // taken out of the live field (g_stickTaken)
std::uint32_t                                 g_stickUser    = 0;        // the player whose scopes see it
bool                                          g_stickTaken   = false;    // the live field was emptied after the tick

/// A count for each further pad, by its number (pad 2 is XInput user 1): "pad 2 n, pad 3 m, pad 4 k".
std::wstring PerPad(const std::uint32_t (&counts)[kMaxPads])
{
    std::wstring text;
    for (unsigned user = 1; user < kMaxPads; ++user)
    {
        text += std::format(L"{}pad {} {}", user == 1 ? L"" : L", ", user + 1, counts[user]);
    }
    return text;
}

/// The same, each count less its value at `since` (a count never resets, so the difference is the span's own).
std::wstring PerPadSince(const std::uint32_t (&counts)[kMaxPads], const std::uint32_t (&since)[kMaxPads])
{
    std::uint32_t delta[kMaxPads]{};
    for (unsigned user = 0; user < kMaxPads; ++user)
    {
        delta[user] = counts[user] - since[user];
    }
    return PerPad(delta);
}

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
    const bool here = g_thread.Here();
    if (!here && !g_offThreadLogged)
    {
        g_offThreadLogged = true;
        platform::LogWarning(L"pad buttons: the input object was driven off the game thread; that call is passed through");
    }
    return here;
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

std::wstring Names(std::span<const std::uint64_t> names) noexcept
try
{
    std::wstring text;
    for (const std::uint64_t name : names)
    {
        const std::wstring one = platform::NameText(name);
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

/// A minigame or a bike chase holds the whole screen (this engine tick's read: the input tick runs before it, so the
/// first frame of a moment and the first after it still take the frame before's route; a key's release follows
/// its press either way).
bool PlayTime() noexcept
{
    return g_minigames && menu_pads::SharedPlay(views::whole_screen::Current());
}

/// The game's input is shared: every pad is the game's pad. A pause menu over a minigame is a menu's moment (the
/// whole-screen reason ranks the menu first), so with the menus' share off it ends the shared moment.
bool SharedTime() noexcept
{
    return MenuTime() || PlayTime();
}

/// The Info line's text at a shared moment's beginning or end; empty if it could not be formatted (the input tick
/// must not throw).
std::wstring SharedMomentText(bool starting) noexcept
try
{
    if (!starting)
    {
        return std::format(L"the shared moment ended; each seat's pad's buttons are its own player's again (key events "
                           L"the game handled as its own pad's during it: {})",
                           PerPadSince(g_toGame, g_toGameAtStart));
    }
    if (MenuTime())
    {
        return L"a menu owns input; every pad drives it";
    }
    return std::format(L"{} holds the screen; every pad drives it", views::whole_screen::NameOf(views::whole_screen::Current()));
}
catch (...)
{
    return {};
}

struct Handled
{
    std::int64_t result = 0;
    bool         inCopy = false;  // false: the copy could not be exchanged in and the game's own state took it
};

/// `call` (a game handler or dispatch) as `user`'s event, its copy already exchanged in: the dispatcher's flag and the
/// release route know the player, and with `ui` the press runs in that player's UI context when one opens.
template <class Call>
auto AsPlayersEvent(unsigned user, bool ui, Call&& call)
{
    g_eventUser   = user;
    g_eventRouted = ui && g_ui.enter != nullptr && g_ui.enter(user);
    const auto result = call();
    if (g_eventRouted && g_ui.leave != nullptr)
    {
        g_ui.leave(user);
    }
    g_eventRouted = false;
    g_eventUser   = 0;
    return result;
}

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
    const std::int64_t result = AsPlayersEvent(user, ui, [&] { return original(self, app, event); });
    SwapBack(user);
    return {result, true};
}

/// One pad's key event as the game's own pad event: its tables, its dispatcher's own argument, its UI route.
std::int64_t ToGame(KeyEventFn original, void* self, void* app, const std::uint8_t* event, unsigned user) noexcept
{
    ++g_toGame[user];
    return original(self, app, event);
}

/// A further player's key-down (`original`: the key-down or mouse-button handler; `key` the key's or button's name):
/// a key already down repeats where its press went; a new press goes to the game while the game's input is shared,
/// otherwise to the player's copy, and also to the game when it pressed only menu openers there.
std::int64_t PadDown(KeyEventFn original, void* self, void* app, const std::uint8_t* event, unsigned user, std::uint64_t key)
{
    menu_pads::Keys&   keys     = g_keys[user];
    const bool         shared   = SharedTime();
    const bool         newPress = menu_pads::Where(keys, key) == 0;
    std::uint8_t       route    = menu_pads::Route(keys, key, shared);
    std::int64_t       result   = 0;
    if ((route & menu_pads::kToCopy) != 0)
    {
        const bool check = g_menus && newPress && !shared && g_openers[0] != 0;
        const Handled handled = InCopy(original, self, app, event, user, !shared, check);
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
            if (platform::Enabled(platform::Level::Debug))
            {
                platform::LogDebug(L"pad buttons: pad {}'s press of {} set {} in its own state", user + 1,
                                   Names(std::span<const std::uint64_t>(&key, 1)),
                                   g_pressed.empty() ? std::wstring(L"no action") : Names(g_pressed));
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
        const std::int64_t game = ToGame(original, self, app, event, user);
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

/// A further player's key-up (`original`: the key-up or mouse-button-up handler): released where its press went (a key
/// never recorded: the game's while the game's input is shared, otherwise the player's copy).
std::int64_t PadUp(KeyEventFn original, void* self, void* app, const std::uint8_t* event, unsigned user, std::uint64_t key)
{
    menu_pads::Keys&   keys     = g_keys[user];
    const std::uint8_t route    = menu_pads::Route(keys, key, SharedTime());
    menu_pads::Forget(keys, key);
    std::int64_t result = 0;
    if ((route & menu_pads::kToCopy) != 0)
    {
        // The release keeps the player's UI context even in a shared moment: its press may have been captured there.
        const Handled handled = InCopy(original, self, app, event, user, true, false);
        if (!handled.inCopy)
        {
            return handled.result;  // the game's own state took the release
        }
        result = handled.result;
    }
    if ((route & menu_pads::kToGame) != 0)
    {
        const std::int64_t game = ToGame(original, self, app, event, user);
        if ((route & menu_pads::kToCopy) == 0)
        {
            result = game;
        }
    }
    return result;
}

/// The player the keyboard and mouse play while it is player 2 or 3 with their own buttons, else -1 (player 1's: the
/// game's own state, as in single play).
std::int32_t KeyboardRoute() noexcept
{
    const int user = pad_table::KeyboardUser();
    return OwnPad(static_cast<unsigned>(user)) ? user : -1;
}

/// Views split on screen: one view (a scene, a menu, the title) shows the game's own last device.
bool SplitShown() noexcept
{
    return views::column_layout::Views() >= 2 && !views::whole_screen::Shown();
}

/// `call` (a key, mouse button or wheel event in the game's handler) with the game's "keyboard used" flag put back as it
/// was, while the keyboard plays player 2 or 3 and views are split: the flag makes the keyboard the last device for
/// everyone (every view's button art, the rumble's device), which stays player 1's device's there. With one view on
/// screen, the keyboard marks itself as any device does.
template <class Call>
std::int64_t KeepingKeyboardFlag(Call&& call) noexcept
{
    std::uint8_t* const owner  = Owner();
    std::uint8_t        before = 0;
    const bool          keep   = owner != nullptr && KeyboardRoute() >= 1 && SplitShown() &&
                        platform::Read(owner + facts::kInputKeyboardUsed, before);
    const std::int64_t  result = call();
    std::uint8_t        after  = 0;
    if (keep && platform::Read(owner + facts::kInputKeyboardUsed, after) && after != before)
    {
        platform::WriteGuarded(owner + facts::kInputKeyboardUsed, &before, sizeof(before));
        ++g_flagsKept;
    }
    return result;
}

/// The game's note of the pad that pressed as its last device used (its button art and the rumble's device): while
/// views are split, only player 1's slot notes itself, so every view's game art and player 1's trigger effects stay
/// player 1's device's (each other view draws its own player's art, hud/button_art). With one view on screen every pad
/// notes itself, as in single play.
void NoteLastDevice(void* owner, std::int32_t controllerId)
{
    if (controllerId != 0 && platform::OnGameThread() && SplitShown())
    {
        ++g_notesKept;
        return;
    }
    reinterpret_cast<void (*)(void*, std::int32_t)>(g_note.Original())(owner, controllerId);
}

/// Said when the player the keyboard's events go to changes.
void NoteKeyboardPlayer(std::int32_t user) noexcept
{
    const int now = user >= 1 ? user : 0;
    if (now != g_routedSaid)
    {
        g_routedSaid = now;
        platform::Log(L"pad buttons: the keyboard's keys and the mouse's buttons press player {}'s buttons{}", now + 1,
                      now == 0 ? L" (the game's own state)" : L" (that player's own state)");
    }
}

/// `user` (where a keyboard key's or mouse button's press went) is player 2's or 3's with their own buttons.
bool KeyboardPlaysSeat(std::int32_t user) noexcept
{
    return user >= 1 && OwnPad(static_cast<unsigned>(user)) && OnGameThread();
}

/// A keyboard key's or mouse button's down (`original` its handler, `held` the keys' or the buttons' table, `key` its
/// name) to the player it plays: its press, repeats and release all where the press went (keyboard_route_rules).
std::int64_t KeyboardDown(KeyEventFn original, DownKeys& held, void* self, void* app, const std::uint8_t* event,
                          std::uint64_t key, bool repeat)
{
    const auto kind = repeat ? keyboard_route_rules::KeyEvent::Other : keyboard_route_rules::KeyEvent::Press;
    const std::int32_t user = keyboard_route_rules::KeyId(held, key, kind, 0, KeyboardRoute());
    if (!repeat)
    {
        NoteKeyboardPlayer(user);
    }
    return KeepingKeyboardFlag([&] {
        if (KeyboardPlaysSeat(user))
        {
            ++g_keyboardEvents;
            return PadDown(original, self, app, event, static_cast<unsigned>(user), key);
        }
        return original(self, app, event);
    });
}

/// A keyboard key's or mouse button's up, released where its press went.
std::int64_t KeyboardUp(KeyEventFn original, DownKeys& held, void* self, void* app, const std::uint8_t* event,
                        std::uint64_t key)
{
    const std::int32_t user =
        keyboard_route_rules::KeyId(held, key, keyboard_route_rules::KeyEvent::Release, 0, KeyboardRoute());
    if (KeyboardPlaysSeat(user))
    {
        ++g_keyboardEvents;
        return PadUp(original, self, app, event, static_cast<unsigned>(user), key);
    }
    return original(self, app, event);
}

/// What the key handlers read of a key event.
struct KeyEventFields
{
    std::uint32_t user     = 0;      // the Slate index: a pad's controller id; 0 for the keyboard as well
    std::uint64_t key      = 0;      // the key's name
    std::uint32_t keyCode  = 0;      // the Windows key code (none on a pad button's event)
    bool          repeat   = false;  // a system repeat
    bool          keyboard = false;  // a keyboard key's event (index 0 and a key code), on the game thread
};

KeyEventFields ReadKeyEvent(const std::uint8_t* event) noexcept
{
    KeyEventFields fields;
    std::uint8_t   repeatByte = 0;
    std::memcpy(&fields.user, event + facts::kKeyEventUserIndex, sizeof(fields.user));
    std::memcpy(&fields.key, event + facts::kKeyEventKey, sizeof(fields.key));
    std::memcpy(&fields.keyCode, event + facts::kKeyEventKeyCode, sizeof(fields.keyCode));
    std::memcpy(&repeatByte, event + facts::kKeyEventRepeat, sizeof(repeatByte));
    fields.repeat   = repeatByte != 0;
    fields.keyboard = fields.user == 0 && pad_rules::IsKeyboardKey(fields.keyCode) && platform::OnGameThread();
    return fields;
}

/// A mouse event's button name, on the game thread; false elsewhere or unreadable (the event then passes as it came).
bool PointerButton(const std::uint8_t* event, std::uint64_t& button) noexcept
{
    return platform::OnGameThread() && platform::Read(event + facts::kPointerEventButton, button);
}

/// A short Tab's release, a frame after its press reached the game (`now`: at once, before another tap or at the mod's
/// end).
void ReleaseTap(void* self, bool now) noexcept
{
    const std::uint8_t* const due = keyboard_keys::DueRelease(now);
    if (due == nullptr)
    {
        return;
    }
    std::uint64_t key = 0;
    std::memcpy(&key, due + facts::kKeyEventKey, sizeof(key));
    KeyboardUp(g_keyUp, g_keyboardDown, self, g_tapApp, due, key);
    keyboard_keys::FreeRelease();
}

/// The key-down handler: a keyboard key first to keyboard_keys (Tab, the open window's keys), then to the player the
/// keyboard plays; player 2's or 3's pad's to that player's copy; everything else as it came.
std::int64_t KeyDown(void* self, void* app, const std::uint8_t* event)
{
    const KeyEventFields e = ReadKeyEvent(event);
    if (e.keyboard)
    {
        if (!e.repeat)
        {
            keyboard_keys::NotePress();
        }
        if (keyboard_keys::OnDown(e.keyCode, e.repeat) == keyboard_keys::Down::Taken)
        {
            return 1;
        }
        return KeyboardDown(g_keyDown, g_keyboardDown, self, app, event, e.key, e.repeat);
    }
    if (!OwnPad(e.user) || !OnGameThread())
    {
        return g_keyDown(self, app, event);
    }
    return PadDown(g_keyDown, self, app, event, e.user, e.key);
}

/// The key-up handler: a key the mod took is its own (a short Tab's press reaches the game now, its release a frame
/// later); every other up where its press went.
std::int64_t KeyUp(void* self, void* app, const std::uint8_t* event)
{
    const KeyEventFields e = ReadKeyEvent(event);
    if (e.keyboard)
    {
        const keyboard_keys::Up up = keyboard_keys::OnUp(e.keyCode);
        if (up == keyboard_keys::Up::Taken)
        {
            return 1;
        }
        if (up == keyboard_keys::Up::Tap)
        {
            ReleaseTap(self, true);  // a tap still kept from just before: its release first
            KeyboardDown(g_keyDown, g_keyboardDown, self, app, event, e.key, false);
            if (keyboard_keys::KeepRelease(event))
            {
                g_tapApp = app;
                return 1;
            }
            // No copy kept: the key is released at once, below.
        }
        return KeyboardUp(g_keyUp, g_keyboardDown, self, app, event, e.key);
    }
    if (!OwnPad(e.user) || !OnGameThread())
    {
        if (e.user >= 1 && e.user < kMaxPads && g_thread.Here())  // no off-thread warning for pad 1
        {
            menu_pads::Forget(g_keys[e.user], e.key);
        }
        return g_keyUp(self, app, event);
    }
    return PadUp(g_keyUp, self, app, event, e.user, e.key);
}

/// The mouse-button handlers: the keyboard's player's, as its keys are; while that player's window is open a click
/// reaches no one, as a key does (a button held from before still lets go where it was pressed).
std::int64_t MouseDown(void* self, void* app, const std::uint8_t* event)
{
    const auto    original = reinterpret_cast<KeyEventFn>(g_mouseDownSlot.Original());
    std::uint64_t button   = 0;
    if (!PointerButton(event, button))
    {
        return original(self, app, event);
    }
    keyboard_keys::NotePress();
    if (keyboard_keys::Idle())
    {
        return 0;
    }
    return KeyboardDown(original, g_mouseHeld, self, app, event, button, false);
}

std::int64_t MouseUp(void* self, void* app, const std::uint8_t* event)
{
    const auto    original = reinterpret_cast<KeyEventFn>(g_mouseUpSlot.Original());
    std::uint64_t button   = 0;
    if (!PointerButton(event, button))
    {
        return original(self, app, event);
    }
    return KeyboardUp(original, g_mouseHeld, self, app, event, button);
}

/// The wheel handler: the wheel steps UI focus only, so the keyboard's player turns it in their own UI context, and in
/// none (the wheel dropped) while they have none open, rather than in player 1's; while the game's input is shared it
/// turns the game's.
std::int64_t Wheel(void* self, void* app, const std::uint8_t* event)
{
    const auto         original = reinterpret_cast<KeyEventFn>(g_wheelSlot.Original());
    const std::int32_t user     = platform::OnGameThread() ? KeyboardRoute() : -1;
    if (platform::OnGameThread() && keyboard_keys::Idle())
    {
        return 0;  // the keyboard's player's window is open
    }
    if (user < 1 || SharedTime() || !OnGameThread())
    {
        return KeepingKeyboardFlag([&] { return original(self, app, event); });
    }
    if (g_ui.enter == nullptr || !g_ui.enter(static_cast<unsigned>(user)))
    {
        ++g_wheelsDropped;
        return 0;
    }
    const std::int64_t result = KeepingKeyboardFlag([&] { return original(self, app, event); });
    if (g_ui.leave != nullptr)
    {
        g_ui.leave(static_cast<unsigned>(user));
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

/// The player the held mouse buttons' presses went to (the first held one's), or -1 with none held.
std::int32_t HeldMousePlayer() noexcept
{
    for (const keyboard_route_rules::DownKey& button : g_mouseHeld)
    {
        if (button.name != 0)
        {
            return button.id;
        }
    }
    return -1;
}

/// The input tick's repeat of a held mouse button (a direct dispatcher call): the player the held button's press went
/// to, in their copy and their UI context; the game's while its input is shared.
char MouseRepeat(void* owner, void* unused, void* actionEvent, std::uint8_t alreadyHandled)
{
    const auto         original = reinterpret_cast<DispatchFn>(g_mouseRepeat.Original());
    const std::int32_t user     = platform::OnGameThread() ? HeldMousePlayer() : -1;
    if (user < 1 || !OwnPad(static_cast<unsigned>(user)) || SharedTime() || !OnGameThread() ||
        !Swap(static_cast<unsigned>(user)))
    {
        return original(owner, unused, actionEvent, alreadyHandled);
    }
    const auto seat = static_cast<unsigned>(user);
    ++g_keyboardEvents;
    const char result = AsPlayersEvent(seat, true, [&] { return original(owner, unused, actionEvent, alreadyHandled); });
    SwapBack(seat);
    return result;
}

/// The keyboard's direction and trigger bits, kept out of one input tick.
struct KeyboardBits
{
    std::array<std::uint8_t, facts::kStickDigitalBitsBytes>    directions{};
    std::array<std::uint8_t, facts::kTriggerDigitalBitsBytes>  triggers{};
    bool                                                       out = false;
};

/// Before the input tick: while the keyboard plays player 2 or 3 and the game's input is not shared, its direction and
/// trigger bits are taken out for the tick. The tick builds the digital stick and triggers from them (the game's stick
/// getter prefers them to the pads'), and its stick also steers the live UI focus, player 1's: without them both are
/// player 1's pad alone, as the stick banks are.
KeyboardBits StripKeyboardBits(std::uint8_t* owner, bool shared) noexcept
{
    KeyboardBits bits;
    const std::int32_t route = KeyboardRoute();
    g_stickUser  = route >= 1 ? static_cast<std::uint32_t>(route) : 0;
    g_stickTaken = false;
    if (route < 1 || shared)
    {
        return bits;
    }
    std::memcpy(bits.directions.data(), owner + facts::kStickDigitalBits, bits.directions.size());
    std::memcpy(bits.triggers.data(), owner + facts::kTriggerDigitalBits, bits.triggers.size());
    std::memset(owner + facts::kStickDigitalBits, 0, bits.directions.size());
    std::memset(owner + facts::kTriggerDigitalBits, 0, bits.triggers.size());
    bits.out = true;
    return bits;
}

/// After the input tick: the bits back (with any the tick's own mouse repeat set meanwhile), and the keyboard's player's
/// digital stick built from them for their scopes (KeyboardStickFor).
void RestoreKeyboardBits(std::uint8_t* owner, const KeyboardBits& bits) noexcept
{
    if (!bits.out)
    {
        return;
    }
    for (std::size_t i = 0; i < bits.directions.size(); ++i)
    {
        owner[facts::kStickDigitalBits + i] |= bits.directions[i];
    }
    for (std::size_t i = 0; i < bits.triggers.size(); ++i)
    {
        owner[facts::kTriggerDigitalBits + i] |= bits.triggers[i];
    }
    g_keyboardStick = keyboard_route_rules::DigitalStick(owner[facts::kStickDigitalBits],
                                                         owner[facts::kStickDigitalBits + facts::kStickDigitalBitsRight]);
    g_stickTaken    = true;
}

/// The input tick: the stick aggregation sees pad 1's record alone (every pad's while the game's input is shared:
/// the game's own aggregate, the furthest-pushed stick wins), then each pad's copy gets the game's advance.
void Tick(void* self, float deltaTime, void* slateApp, void* cursor)
{
    std::uint8_t* owner = static_cast<std::uint8_t*>(self);
    if (platform::OnGameThread())
    {
        ReleaseTap(self, false);
    }
    if (g_pads == 0 || !g_enabled || !OnGameThread())
    {
        g_stickUser  = 0;  // no further player: the keyboard's digital stick is player 1's, as the game left it
        g_stickTaken = false;
        g_tick(self, deltaTime, slateApp, cursor);
        return;
    }
    if (g_openers[0] == 0 && ReadOpeners())
    {
        platform::Log(L"pad buttons: the menu openers' names read; every pad opens the game's menus");
    }
    const bool shared = SharedTime();
    if (shared != g_shared)
    {
        g_shared = shared;
        if (shared)
        {
            std::ranges::copy(g_toGame, g_toGameAtStart);
        }
        platform::Log(L"pad buttons: {}", SharedMomentText(shared));
    }
    // Both readers' banks (XInput's, the PlayStation reader's) to their first record, player 1's slot: the aggregate is
    // player 1's pad alone, whichever reader it is on.
    constexpr std::array<std::uint32_t, 2> kBanks{facts::kXInputBank, facts::kPlayStationBank};
    std::array<std::int32_t, 2>            counts{};
    const std::int32_t                     one = 1;
    for (std::size_t i = 0; i < kBanks.size(); ++i)
    {
        std::uint8_t* const bank = owner + facts::kStickBanks + 16 * kBanks[i] + facts::kStickBankCount;
        std::memcpy(&counts[i], bank, sizeof(counts[i]));
        if (shared)
        {
            counts[i] = 0;  // the game's own aggregate: nothing clamped, nothing to put back
        }
        if (counts[i] > 1)
        {
            std::memcpy(bank, &one, sizeof(one));
        }
    }
    const KeyboardBits keyboardBits = StripKeyboardBits(owner, shared);
    g_tick(self, deltaTime, slateApp, cursor);
    RestoreKeyboardBits(owner, keyboardBits);
    for (std::size_t i = 0; i < kBanks.size(); ++i)
    {
        if (counts[i] > 1)
        {
            std::memcpy(owner + facts::kStickBanks + 16 * kBanks[i] + facts::kStickBankCount, &counts[i], sizeof(counts[i]));
        }
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
    const auto owner    = game_globals::Address(game_globals::Global::SquareInputOwner);
    const auto dispatch = platform::FindFunction(image, facts::kActionDispatch);
    const auto routeUp  = platform::FindFunction(image, facts::kRouteActionUp);
    const auto routeUpCall = image.Rebase(facts::kRouteActionUpCall, 5);
    if (!owner || !dispatch || !routeUp || !routeUpCall)
    {
        platform::Log(L"pad buttons: an engine check failed; every pad presses player 1's buttons");
        return false;
    }
    g_owner = reinterpret_cast<std::uint8_t**>(owner);
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
    // The mouse's buttons, their repeat and the wheel: the keyboard's player's too. The keys play on without them.
    const auto repeatSite = image.Rebase(facts::kMouseRepeatCall, 5);
    g_mouseTaken =
        repeatSite && g_mouseRepeat.Install(*repeatSite, *dispatch, reinterpret_cast<std::uintptr_t>(&MouseRepeat)) &&
        platform::ReplaceSlot(image, facts::kInputMouseDown, reinterpret_cast<std::uintptr_t>(&MouseDown), g_mouseDownSlot) &&
        platform::ReplaceSlot(image, facts::kInputMouseUp, reinterpret_cast<std::uintptr_t>(&MouseUp), g_mouseUpSlot) &&
        platform::ReplaceSlot(image, facts::kInputWheel, reinterpret_cast<std::uintptr_t>(&Wheel), g_wheelSlot);
    const auto note = platform::FindFunction(image, facts::kNoteLastDevice);
    const std::span<const std::uint8_t> noteFirst(facts::kNoteLastDeviceEntry.data(), facts::kNoteLastDeviceFirstBytes);
    if (!note || !g_note.Install(*note, noteFirst, reinterpret_cast<std::uintptr_t>(&NoteLastDevice)))
    {
        platform::LogWarning(L"pad buttons: the game's last-device note could not be taken (a check failed): while views are "
                             L"split, players 2 and 3's pads still turn every view's button art to theirs");
    }
    if (!g_mouseTaken)
    {
        g_wheelSlot.Restore();
        g_mouseUpSlot.Restore();
        g_mouseDownSlot.Restore();
        g_mouseRepeat.Restore();
        platform::LogWarning(L"pad buttons: the mouse's handlers could not be taken (a check failed): with the keyboard on "
                             L"player 2 or 3, the mouse's buttons and wheel still press player 1's");
    }
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
    control::Register({.name  = L"pad-minigames",
                       .shape = control::Shape::OnOff,
                       .help  = L"every pad drives a minigame or a bike chase that holds the screen (players/menu_pads.h; on)",
                       .set   = [](const control::Value& value) {
                           g_minigames = std::get<bool>(value);
                           platform::Log(L"pad minigames {}", g_minigames ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_minigames}; }});
    control::Register({.name = L"pad.to-game", .shape = control::Shape::Text,
                       .help = L"each seat's pad's key events the game handled as its own pad's (menus, minigames, bike chases)",
                       .get  = [] { return control::Value{PerPad(g_toGame)}; }});
    control::Register({.name = L"pad.menu-opens", .shape = control::Shape::Integer, .max = INT32_MAX,
                       .help = L"further pads' presses of Start, the map or the enemy intel button given to the game",
                       .get  = [] { return control::Value{static_cast<std::int64_t>(g_opens)}; }});
    control::Register({.name = L"pad.events", .shape = control::Shape::Text,
                       .help = L"key events handled in each further pad's own state so far",
                       .get  = [] { return control::Value{PerPad(g_events)}; }});
    control::Register({.name = L"pad.keyboard", .shape = control::Shape::Text,
                       .help = L"the keyboard and mouse as a player's device: the player their buttons press, their events "
                               L"in player 2's or 3's own state, the \"keyboard used\" flag put back, wheel turns dropped",
                       .get  = [] {
                           try
                           {
                               return control::Value{std::format(
                                   L"player {}{}; events in player 2's or 3's own state {}; flag put back {}; wheel turns "
                                   L"dropped {}; players 2 and 3's pads kept out of the device note {}",
                                   KeyboardRoute() >= 1 ? KeyboardRoute() + 1 : 1, g_mouseTaken ? L"" : L" (keys only)",
                                   g_keyboardEvents, g_flagsKept, g_wheelsDropped, g_notesKept)};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
    platform::Log(L"pad buttons: installed; each further pad's buttons are its own player's{}; every pad drives a "
                  L"minigame or a bike chase that holds the screen (pad-minigames)",
                  g_openers[0] != 0 ? L"; every pad opens the game's menus and drives an open one (pad-menus)"
                                     : L"; every pad drives an open menu (pad-menus)");
    return true;
}

void Remove() noexcept
{
    // A short Tab's press never outlives the mod: its release now, on the game thread; elsewhere only freed.
    if (std::uint8_t* const owner = Owner(); owner != nullptr && g_keyUp != nullptr && platform::OnGameThread())
    {
        ReleaseTap(owner, true);
    }
    else if (keyboard_keys::DueRelease(true) != nullptr)
    {
        // Off the game thread the copy is left as it is: its destructor touches a count only the game thread may.
        platform::Log(L"pad buttons: a short Tab's release was due at the mod's end off the game thread; left unsent");
    }
    g_note.Restore();
    g_wheelSlot.Restore();
    g_mouseUpSlot.Restore();
    g_mouseDownSlot.Restore();
    g_mouseRepeat.Restore();
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
    g_thread.Set();  // the game thread: joins and leaves run on it, as the input object does
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

int KeyboardPlayer() noexcept
{
    const std::int32_t user = KeyboardRoute();
    return user >= 1 ? user : 0;
}

std::array<float, 4> KeyboardStickFor(std::uint32_t user, const std::array<float, 4>& live) noexcept
{
    if (user != g_stickUser)
    {
        return {};
    }
    return g_stickTaken ? g_keyboardStick : live;
}

}  // namespace lss::players::pad_buttons
