#pragma once

// Which player a key's event goes to (players/keyboard_route, players/pad_buttons). Pure: host-tested
// (tests/keyboard_route_test.cpp).
//
// The keyboard's player may change while a key is down (a join, a leave, a trade): every event of a key held then (its
// system repeats, its release) goes where its press went, or the first player's input would keep the key held, or the
// new player's would.

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::players::keyboard_route_rules {

/// A key down, with the controller id its press went on with.
struct DownKey
{
    std::uint64_t name = 0;  // the key's name (FName), 0 for a free entry
    std::int32_t  id   = 0;
};

enum class KeyEvent : std::uint8_t
{
    Press,
    Release,
    Other,  // a system repeat, a double click
};

/// The controller id `name`'s event goes on with: the id its press took while it is down (forgotten at its release),
/// else the route now (`routed`, -1 for none: the event's own `controllerId`), a press remembered with it. With every
/// entry taken, a press takes the first.
template <std::size_t N>
constexpr std::int32_t KeyId(std::array<DownKey, N>& down, std::uint64_t name, KeyEvent event, std::int32_t controllerId,
                             std::int32_t routed) noexcept
{
    DownKey* same = nullptr;
    DownKey* free = nullptr;
    for (DownKey& key : down)
    {
        same = same == nullptr && name != 0 && key.name == name ? &key : same;
        free = free == nullptr && key.name == 0 ? &key : free;
    }
    if (same != nullptr && event != KeyEvent::Press)
    {
        const std::int32_t id = same->id;
        if (event == KeyEvent::Release)
        {
            *same = DownKey{};
        }
        return id;
    }
    const std::int32_t id = routed >= 0 ? routed : controllerId;
    if (event == KeyEvent::Press && name != 0)
    {
        DownKey* const at = same != nullptr ? same : free != nullptr ? free : &down.front();
        *at               = DownKey{name, id};
    }
    return id;
}

/// The keyboard's digital stick (LX, LY, RX, RY) from its direction bits (the left stick's byte, then the right's), as
/// the game's input tick builds it: per axis one bit alone is ±1, both or neither 0; bit 1 left (-X), bit 2 right
/// (+X), bit 4 up (+Y), bit 8 down (-Y). While the keyboard plays player 2 or 3 its bits are kept out of the tick (whose
/// stick also steers player 1's UI focus), so its player's stick is built here from the same bits.
constexpr std::array<float, 4> DigitalStick(std::uint8_t left, std::uint8_t right) noexcept
{
    const auto axis = [](std::uint8_t bits, std::uint8_t minus, std::uint8_t plus) {
        const bool m = (bits & minus) != 0;
        const bool p = (bits & plus) != 0;
        return m == p ? 0.0f : p ? 1.0f : -1.0f;
    };
    return {axis(left, 1, 2), axis(left, 8, 4), axis(right, 1, 2), axis(right, 8, 4)};
}

}  // namespace lss::players::keyboard_route_rules
