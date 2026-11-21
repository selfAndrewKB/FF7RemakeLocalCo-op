#include "players/keyboard_route.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/keyboard_route_rules.h"
#include "players/pad_buttons.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace lss::players::keyboard_route {

namespace {

namespace rules = keyboard_route_rules;

using InputKeyFn  = bool (*)(void* self, void* viewport, std::int32_t controllerId, void* key, std::int32_t event,
                            float amount, bool gamepad);
using InputAxisFn = bool (*)(void* self, void* viewport, std::int32_t controllerId, void* key, float delta, float deltaTime,
                             std::int32_t samples, bool gamepad);

// Set once at install.
platform::VTableEntry g_keySlot;
platform::VTableEntry g_axisSlot;

// Game thread.
std::array<rules::DownKey, 32> g_down{};  // the keys down, each with the id its press went on with
std::uint32_t                  g_keysRouted = 0, g_axesRouted = 0;
std::int32_t                   g_lastRouted = 0;  // the id the last routed input went on with (its change is said)

/// The controller id the keyboard and mouse go on with: the keyboard's player's while it plays player 2 or 3 alone,
/// else -1 (the engine's own id, player 1's). Only the keyboard's own id, 0, is changed.
std::int32_t RoutedId(std::int32_t controllerId) noexcept
{
    if (controllerId != 0)
    {
        return -1;
    }
    const int keyboard = pad_buttons::KeyboardPlayer();
    return keyboard > 0 ? keyboard : -1;
}

/// Said once each time the routed id changes, the first routed input after it.
void NoteRouted(std::int32_t id) noexcept
{
    if (id != g_lastRouted)
    {
        g_lastRouted = id;
        platform::Log(L"keyboard route: the keyboard and mouse reach controller id {} (player {}) at the viewport", id, id + 1);
    }
}

bool InputKey(void* self, void* viewport, std::int32_t controllerId, void* key, std::int32_t event, float amount,
              bool gamepad) noexcept
{
    const auto    original = reinterpret_cast<InputKeyFn>(g_keySlot.Original());
    std::uint64_t name     = 0;
    if (gamepad || !platform::OnGameThread() || !platform::Read(key, name))
    {
        return original(self, viewport, controllerId, key, event, amount, gamepad);
    }
    const rules::KeyEvent kind = event == facts::kInputEventPressed    ? rules::KeyEvent::Press
                                 : event == facts::kInputEventReleased ? rules::KeyEvent::Release
                                                                       : rules::KeyEvent::Other;
    const std::int32_t    routed = rules::KeyId(g_down, name, kind, controllerId, RoutedId(controllerId));
    if (routed != controllerId)
    {
        ++g_keysRouted;
        NoteRouted(routed);
    }
    // The key by value is the caller's copy, handed on as it came: the callee destroys it.
    return original(self, viewport, routed, key, event, amount, gamepad);
}

bool InputAxis(void* self, void* viewport, std::int32_t controllerId, void* key, float delta, float deltaTime,
               std::int32_t samples, bool gamepad) noexcept
{
    const auto         original = reinterpret_cast<InputAxisFn>(g_axisSlot.Original());
    const std::int32_t routed   = gamepad || !platform::OnGameThread() ? -1 : RoutedId(controllerId);
    if (routed >= 0)
    {
        ++g_axesRouted;
        NoteRouted(routed);
        controllerId = routed;
    }
    return original(self, viewport, controllerId, key, delta, deltaTime, samples, gamepad);
}

std::wstring Reading()
{
    return std::format(L"{}; the keyboard and mouse go on with controller id {}; keys and buttons routed {}, axes routed {}",
                       g_axisSlot.Active() ? L"on" : L"not installed", pad_buttons::KeyboardPlayer(), g_keysRouted,
                       g_axesRouted);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const bool installed =
        platform::ReplaceSlot(image, facts::kViewportInputKey, reinterpret_cast<std::uintptr_t>(&InputKey), g_keySlot) &&
        platform::ReplaceSlot(image, facts::kViewportInputAxis, reinterpret_cast<std::uintptr_t>(&InputAxis), g_axisSlot);
    if (!installed)
    {
        g_keySlot.Restore();
        platform::LogWarning(L"keyboard route: the viewport's input entries could not be taken (a check failed): the mouse "
                             L"look stays player 1's");
        return false;
    }
    control::Register({.name  = L"players.keyboard-route",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) which controller id the keyboard and mouse reach the viewport with, and how "
                                L"many keys and axes went to the keyboard's player",
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
    platform::Log(L"keyboard route: while the keyboard plays player 2 or 3, its keys, the mouse's buttons, wheel and look "
                  L"reach that player's controller (the viewport's own input entries, by controller id)");
    return true;
}

void Remove() noexcept
{
    if (g_axisSlot.Active() && !g_axisSlot.Restore())
    {
        platform::Log(L"keyboard route: the viewport's InputAxis entry was changed after us; left in place (it passes "
                      L"through)");
    }
    if (g_keySlot.Active() && !g_keySlot.Restore())
    {
        platform::Log(L"keyboard route: the viewport's InputKey entry was changed after us; left in place (it passes "
                      L"through)");
    }
}

}  // namespace lss::players::keyboard_route
