#include "players/keyboard_keys.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/steady_ms.h"
#include "platform/verified_code.h"
#include "players/pad_buttons.h"
#include "players/pad_rules.h"

#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <utility>

namespace lss::players::keyboard_keys {

namespace {

namespace rules = pad_rules;

using DestroyEventFn = void* (*)(void* event);

// Set once at install.
DestroyEventFn g_destroyEvent = nullptr;
bool           g_installed    = false;

// A short Tab's release, kept for a later input tick: its press reached the game at the let-go, so the game's
// per-frame "pressed" reads see the press (a press and release in one frame read as neither).
struct TapRelease
{
    alignas(8) std::array<std::uint8_t, facts::kKeyEventBytes> event{};  // a copy of Tab's key-up event, alive while kept
    bool kept   = false;
    int  ticks  = 0;  // input ticks since it was kept
};
TapRelease g_tapRelease;

std::atomic<pad_table::Listener> g_listener{nullptr};

// Game thread (Slate's key events, the input tick and the session's tick).
std::bitset<256> g_heldBack;          // by key code: a down the mod took from the game, whose up is the mod's too
bool             g_idle        = false;
bool             g_tabDown     = false;
bool             g_tabFired    = false;  // the opener fired on this press: no tap at its let-go
bool             g_tabInWindow = false;  // the press came while the window was open: its tap is the window's (nothing)
std::int64_t     g_tabSince    = 0;
bool             g_leaveDown   = false;
bool             g_leaveSeen   = false;  // Backspace went down since the last poll (a tap between two polls counts)
bool             g_pressedAny  = false;  // a key or mouse button went down since the last TakePressed
std::uint16_t    g_pressed     = 0;      // the window's keys pressed since the last poll, as the rules' bits
int              g_returnAsked = 0;      // the player the keyboard played at its period key's last press, until taken
rules::Held      g_leave{};
std::uint32_t    g_opens = 0, g_taps = 0, g_windowKeys = 0, g_returns = 0;

void Tell(std::uint16_t pressed, bool open, bool bTap, bool bHeld) noexcept
{
    if (const pad_table::Listener listener = g_listener.load(std::memory_order_relaxed); listener != nullptr)
    {
        listener(rules::kKeyboardPad, pressed, open, bTap, bHeld);
    }
}

std::wstring Reading()
{
    return std::format(L"{}; window {}, Tab {}; opener held {} time(s), taps given {}, window keys {}, period presses {}",
                       g_installed ? L"on" : L"not installed", g_idle ? L"open" : L"closed", g_tabDown ? L"down" : L"up",
                       g_opens, g_taps, g_windowKeys, g_returns);
}

/// The period key is the keyboard's return key now: the keyboard plays player 2 or 3 and its window is closed.
bool ReturnKeyTaken(std::uint32_t keyCode) noexcept
{
    return keyCode == rules::kReturnKey && !g_idle && pad_buttons::KeyboardPlayer() >= 1;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto destroy = platform::FindFunction(image, facts::kKeyEventDestroy);
    if (!destroy)
    {
        platform::LogWarning(L"keyboard: the game's key-event destructor was not found (a check failed): Tab is never held "
                             L"back, and the keyboard opens no player window and adds no player");
        return false;
    }
    g_destroyEvent = reinterpret_cast<DestroyEventFn>(*destroy);
    g_installed    = true;
    control::Register({.name  = L"players.keyboard",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the keyboard as a player's device: its window, Tab, and the opens, taps, "
                                L"window keys and period presses so far",
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
    platform::Log(L"keyboard: Tab held a second opens the keyboard's player's window, or adds a player on the keyboard (a "
                  L"shorter Tab still reaches the game at its let-go); while that window is open every key is its own "
                  L"(the arrows: character and player number, Q/E: Change View, Enter or Escape: done, Backspace held: "
                  L"leave); while it plays player 2 or 3, the period key brings that player's character back to player 1's "
                  L"(in a fight, into the fight)");
    return true;
}

void Remove() noexcept
{
    g_listener.store(nullptr);
    g_installed = false;
    g_idle      = false;
    g_heldBack.reset();
    g_tabDown     = false;
    g_tabFired    = false;
    g_leaveDown   = false;
    g_leaveSeen   = false;
    g_returnAsked = 0;
}

Down OnDown(std::uint32_t keyCode, bool repeat) noexcept
{
    // Tab (the opener, its system repeats too), every key while the window is open, and the return key; but not a
    // repeat of a key held from before (its down reached the game, so its up must too). A repeat of a key the mod took
    // stays the mod's, whatever changed since its press.
    if (!g_installed || keyCode >= g_heldBack.size())
    {
        return Down::Game;
    }
    if (repeat && g_heldBack.test(keyCode))
    {
        return Down::Taken;
    }
    const bool heldFromBefore = repeat;  // a repeat here is of a key the mod did not take
    if (ReturnKeyTaken(keyCode) && !heldFromBefore)
    {
        g_heldBack.set(keyCode);
        g_returnAsked = pad_buttons::KeyboardPlayer();
        ++g_returns;
        platform::LogDebug(L"keyboard: the period key: player {} asks to be brought back", g_returnAsked + 1);
        return Down::Taken;
    }
    if (!(keyCode == rules::kOpenKey || (g_idle && !heldFromBefore)))
    {
        return Down::Game;
    }
    g_heldBack.set(keyCode);
    if (keyCode == rules::kOpenKey)
    {
        if (!g_tabDown)
        {
            g_tabDown     = true;
            g_tabFired    = false;
            g_tabInWindow = g_idle;
            g_tabSince    = platform::NowMs();
        }
    }
    else if (!repeat)
    {
        g_pressed = static_cast<std::uint16_t>(g_pressed | rules::FromKey(keyCode));
        if (keyCode == rules::kLeaveKey)
        {
            g_leaveDown = true;
            g_leaveSeen = true;
        }
        ++g_windowKeys;
    }
    return Down::Taken;
}

Up OnUp(std::uint32_t keyCode) noexcept
{
    if (!g_installed || keyCode >= g_heldBack.size() || !g_heldBack.test(keyCode))
    {
        return Up::Game;
    }
    g_heldBack.reset(keyCode);
    if (keyCode == rules::kOpenKey)
    {
        const bool tap = g_tabDown && !g_tabFired && !g_tabInWindow && !g_idle;
        g_tabDown      = false;
        g_tabFired     = false;
        if (tap)
        {
            ++g_taps;
            return Up::Tap;
        }
    }
    else if (keyCode == rules::kLeaveKey)
    {
        g_leaveDown = false;
    }
    return Up::Taken;
}

bool KeepRelease(const std::uint8_t* keyEvent) noexcept
{
    if (g_tapRelease.kept || !platform::CopyGuarded(g_tapRelease.event.data(), keyEvent, g_tapRelease.event.size()))
    {
        return false;
    }
    // The copy holds one more reference to the key's details, as the game's own copy of a key event does, and no widget
    // path, as the game's fresh events have none; the game's destructor lets the reference go (FreeRelease).
    std::memset(g_tapRelease.event.data() + facts::kKeyEventPath, 0, sizeof(void*));
    void*        controller = nullptr;
    std::int32_t count      = 0;
    std::memcpy(&controller, g_tapRelease.event.data() + facts::kKeyEventKeyReference, sizeof(controller));
    if (controller != nullptr)
    {
        auto* const at = static_cast<std::uint8_t*>(controller) + facts::kSharedReferenceCount;
        if (!platform::Read(at, count))
        {
            return false;
        }
        ++count;
        if (!platform::WriteGuarded(at, &count, sizeof(count)))
        {
            return false;
        }
    }
    g_tapRelease.kept  = true;
    g_tapRelease.ticks = 0;
    return true;
}

const std::uint8_t* DueRelease(bool now) noexcept
{
    if (!g_tapRelease.kept || (!now && ++g_tapRelease.ticks < 2))
    {
        return nullptr;
    }
    return g_tapRelease.event.data();
}

void FreeRelease() noexcept
{
    if (!g_tapRelease.kept)
    {
        return;
    }
    g_tapRelease.kept = false;
    if (g_destroyEvent != nullptr)
    {
        g_destroyEvent(g_tapRelease.event.data());
    }
}

void NotePress() noexcept
{
    g_pressedAny = true;
}

bool Idle() noexcept
{
    return g_idle;
}

bool TakePressed() noexcept
{
    return std::exchange(g_pressedAny, false);
}

int TakeReturn() noexcept
{
    return std::exchange(g_returnAsked, 0);
}

void Listen(pad_table::Listener listener) noexcept
{
    g_listener.store(listener);
}

void Poll() noexcept
{
    if (!g_installed)
    {
        return;
    }
    const std::int64_t now  = platform::NowMs();
    const bool         open = g_tabDown && !g_tabFired && now - g_tabSince >= rules::kOpenHoldMs;
    if (open)
    {
        g_tabFired = true;
        ++g_opens;
        platform::Log(L"keyboard: Tab held 1 s");
    }
    const rules::Long   leave   = rules::Hold(g_leave, g_leaveDown || std::exchange(g_leaveSeen, false), now,
                                              rules::kLeaveHoldMs);
    const std::uint16_t pressed = std::exchange(g_pressed, 0);
    if (pressed != 0 || open || leave != rules::Long::None)
    {
        Tell(pressed, open, leave == rules::Long::Tap, leave == rules::Long::Hold);
    }
}

void SetIdle(bool idle) noexcept
{
    if (idle == g_idle)
    {
        return;
    }
    g_idle = idle;
    if (!idle)
    {
        g_pressed   = 0;
        g_leaveDown = false;
        g_leaveSeen = false;
        g_leave     = {};
    }
    platform::LogDebug(L"keyboard: {}", idle ? L"its player's window is open: every key is the window's"
                                             : L"its player's window closed: the keys reach the game again");
}

}  // namespace lss::players::keyboard_keys
