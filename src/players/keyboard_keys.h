#pragma once

// The keyboard as one more device in the player windows' input (players/player_window), as players/pad_table makes each
// pad one (research/RE-kbm-routing.md).
//
// Square's key handlers are players/pad_buttons' wrappers; for a keyboard key they ask here first:
//   - Tab, the opener (as View is a pad's): its press is held back from the game. Held a second, it is the keyboard's
//     opener, told to the listener as a pad's View hold is; let go sooner, the game gets the press then (the key-down
//     handler with the key-up's event, as the game's own key repeat presses a key) and its release a frame later, so
//     a game binding on Tab still works, a tap's length late (a press and release in one frame read as neither);
//   - while the keyboard's player's window is open (SetIdle), every key goes to the window and none to the game, as a
//     pad reads idle then: the keys that stand for the pad's (pad_rules::FromKey) are told to the listener as that
//     pad's presses, Backspace as B's tap or hold. A key held from before the window opened is let go to the game as
//     it came.
// The keyboard is pad_rules::kKeyboardPad in the listener's events. Every key or mouse button pressed is also noted
// for the title screen, where it makes the keyboard player 1 (TakePressed).

#include "players/pad_table.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::keyboard_keys {

/// Finds the game's key-event destructor (the kept release is freed with it). False (logged) if a check fails: Tab is
/// then never held back, and the keyboard opens no window and joins no one.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// What the key-down handler does with a keyboard key's down.
enum class Down : std::uint8_t
{
    Game,   // the game's, routed as the keyboard plays (players/pad_buttons)
    Taken,  // the mod's: never the game's
};
Down OnDown(std::uint32_t keyCode, bool repeat) noexcept;

/// What the key-up handler does with a keyboard key's up.
enum class Up : std::uint8_t
{
    Game,
    Taken,
    Tap,  // the game gets the key's press now, from this up's event, and its release a frame later (KeepRelease)
};
Up OnUp(std::uint32_t keyCode) noexcept;

/// A tap's release, kept: a copy of the key-up event `keyEvent`, handed to the key-up handler a frame later
/// (DueRelease). False (nothing kept) while one is kept already or the copy fails: the caller releases the key at once.
bool KeepRelease(const std::uint8_t* keyEvent) noexcept;

/// The kept release's event once a frame has passed since KeepRelease (`now`: at once, at the mod's end), else null.
/// Once per input tick, before the game's; FreeRelease after handing it on.
const std::uint8_t* DueRelease(bool now) noexcept;
void                FreeRelease() noexcept;

/// A keyboard key or mouse button went down (not a system repeat).
void NotePress() noexcept;

/// A key or mouse button went down since the last call. Game thread.
bool TakePressed() noexcept;

/// The player windows' input from the keyboard (the same events as the pads', pad_table::Listen). Game thread.
void Listen(pad_table::Listener listener) noexcept;

/// Once per engine tick: Tab held a second opens, Backspace's tap or hold, the window's keys. Game thread.
void Poll() noexcept;

/// The keyboard's player's window is open: every key is the window's. Game thread.
void SetIdle(bool idle) noexcept;

/// The keyboard's player's window is open (SetIdle): the mouse's presses reach no one either. Game thread.
bool Idle() noexcept;

}  // namespace lss::players::keyboard_keys
