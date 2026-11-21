#pragma once

// Which button art a player's device gets, by the game's own rule (research/RE-per-player-icons.md). Pure: host-tested
// (tests/button_art_test.cpp).
//
// A text block turns each `<button=NAME>` tag into art from its font: a pad family, a keyboard flag and five key maps.
// The game's device-change listener writes them into its three button fonts from three bytes of the input object: the
// device class (0 a PlayStation pad, 1 the keyboard and mouse, 2 an XInput pad, 3 DirectInput), the sub-class (the pad
// family a keyboard shows its `,pad` tags in) and whether the PlayStation pad is a DualShock 4. Giving the listener a
// player's bytes gives that player the art the game would show if their device were the last one used.

#include <cstdint>

namespace lss::hud::button_art_rules {

inline constexpr std::uint8_t kClassPlayStation = 0;
inline constexpr std::uint8_t kClassKeyboard    = 1;
inline constexpr std::uint8_t kClassXInput      = 2;
inline constexpr std::uint8_t kSubClassPlayStation = 0;  // the sub-classes the game's input tick writes with classes 0 and 2
inline constexpr std::uint8_t kSubClassXInput      = 1;

/// The three bytes the listener reads.
struct DeviceArt
{
    std::uint8_t deviceClass = kClassXInput;
    std::uint8_t subClass    = kSubClassXInput;
    std::uint8_t dualShock4  = 0;

    friend constexpr bool operator==(const DeviceArt&, const DeviceArt&) = default;
};

enum class Device : std::uint8_t
{
    XInputPad,       // always Xbox art: the game keeps no per-pad type for XInput
    PlayStationPad,  // the game's own PlayStation reader: its kind asked per pad
    Keyboard,
};

/// The bytes for a player on `device` (`dualShock4`: a PlayStation pad's own kind), given the game's own bytes now
/// (`game`, player 1's device): a keyboard keeps the game's sub-class, so its `,pad` tags show the pad art player 1's
/// view shows; a byte the listener does not read for the class is kept as the game's.
constexpr DeviceArt ArtOf(Device device, bool dualShock4, DeviceArt game) noexcept
{
    switch (device)
    {
    case Device::Keyboard: return {kClassKeyboard, game.subClass, game.dualShock4};
    case Device::PlayStationPad: return {kClassPlayStation, kSubClassPlayStation, static_cast<std::uint8_t>(dualShock4 ? 1 : 0)};
    case Device::XInputPad: break;
    }
    return {kClassXInput, kSubClassXInput, game.dualShock4};
}

/// The listener draws the same art for `a` as for `b`: the same class and the byte that class reads (a PlayStation pad's
/// kind, the keyboard's sub-class; an XInput or DirectInput pad reads neither).
constexpr bool SameArt(const DeviceArt& a, const DeviceArt& b) noexcept
{
    if (a.deviceClass != b.deviceClass)
    {
        return false;
    }
    switch (a.deviceClass)
    {
    case kClassPlayStation: return a.dualShock4 == b.dualShock4;
    case kClassKeyboard: return a.subClass == b.subClass;
    default: return true;
    }
}

}  // namespace lss::hud::button_art_rules
