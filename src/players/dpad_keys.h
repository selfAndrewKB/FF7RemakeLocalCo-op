#pragma once

// The d-pad's four keys as the engine names them (EKeys::Gamepad_DPad_*, facts::kKeyDPad*), in one place. Each is an
// FKey global whose first 8 bytes are the key's FName: a key event's name compares against them, and the game's
// key-press reads take the FKey itself. Rebased at the install of every module that reads them (the same live
// addresses each time); the name check runs on the game thread once the engine's name table answers.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::dpad_keys {

enum class Dpad : std::uint8_t
{
    Up,
    Down,
    Left,
    Right,
};
inline constexpr Dpad kDpadKeys[] = {Dpad::Up, Dpad::Down, Dpad::Left, Dpad::Right};

/// Rebases the four globals. False when one is outside the image; the keys then read as 0.
bool Install(const platform::GameImage& image) noexcept;

/// The key's FKey global (live), 0 before install or outside the image.
std::uintptr_t Key(Dpad key) noexcept;

/// The key's FName (its global's first 8 bytes); false when unread.
bool Name(Dpad key, std::uint64_t& out) noexcept;

/// Game thread: the four globals carry the names the research read. Asked again on later frames while the engine's name
/// table cannot give a name yet (for a while); false for good, logged once, when a name differs or never comes.
bool Named() noexcept;

}  // namespace lss::players::dpad_keys
