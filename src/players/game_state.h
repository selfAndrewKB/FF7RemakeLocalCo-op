#pragma once

// The game's own state, read for the console and the log: Square Enix's EGameState byte on AEndGameState (title,
// loading, play, pause, menu, leaving), its menu kind and pause flag, and the field step machine on the field root
// (13 = steady field). Read guarded every tick; every change is logged (the state at Info, the rest at Debug), and
// the values are console readings (`game.state`, `game.paused`, `game.menu-kind`, `field.step`, `field.running`),
// so a script or tools/game.py waits on the game's state instead of sleeping. Research: RE-lifecycle-world.md.
// Game thread only.

#include "platform/image.h"

#include <cstdint>
#include <string>

namespace lss::players::game_state {

/// Rebases the globals and registers the readings. False (logged) if a global is outside the image.
bool Install(const platform::GameImage& image) noexcept;

/// Reads the values and logs what changed. Once per engine tick, before the console runs its commands.
void Tick() noexcept;

struct Snapshot
{
    bool          valid   = false;  // the game state object was readable
    std::uint8_t  state   = 0;      // EGameState
    std::uint8_t  menu    = 0;      // the menu kind
    bool          paused  = false;
    bool          hasRoot = false;  // the field root exists
    std::int32_t  step    = -1;     // the field step, -1 without a root
    bool          running = false;
    std::wstring  location;         // the location FName as text ("" without a root or a name)
};

/// The last values Tick read (a reference to them: copy what is kept).
const Snapshot& Current() noexcept;

/// The world the game mode names (AEndGameMode::GetWorld), or null. Game thread.
void* World() noexcept;
/// The world `object` lives in (UObject::GetWorld through its vtable, the call guarded), or null. Game thread.
void* WorldOf(void* object) noexcept;

/// Whether a menu owns the game's input now (research/RE-pause-menu-any-pad.md): the game state is the cinema pause,
/// a menu or the chapter-start pause (7, 8, 10), a shop is up (its UI's own byte: shops run in the field state,
/// RE-shop-two.md), or the game's own IsPauseMenuClosed says a pause-family menu is up (it also catches a
/// Blueprint-opened pause menu and Libra opened from the main menu). Read live, not from the
/// tick's values. False while neither can be read. Game thread.
bool MenuOwnsInput() noexcept;

/// The shop UI's own "a shop is up" byte (EndUISystem[44]+0x370, RE-shop-two.md): a shop runs in the field state.
/// False while unread. Game thread.
bool ShopUp() noexcept;

/// The game's own IsPauseMenuClosed says a pause-family menu is alive (a Blueprint-opened pause menu, Libra from the
/// main menu). False while its check failed or the call raises. Game thread.
bool PauseWidgetAlive() noexcept;

/// The state's console word: none, boot, title, loading, play, pause, menu, leaving, chapter-start, other.
const wchar_t* StateName(std::uint8_t state) noexcept;

}  // namespace lss::players::game_state
