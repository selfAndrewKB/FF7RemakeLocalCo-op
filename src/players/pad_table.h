#pragma once

// Which physical pad answers each of the game's pad slots (plan/topics/input.md; research/RE-pad-slots.md).
//
// The game reads pads in one place, Unreal's stock XInputInterface::SendControllerEvents, through xinput1_3's two
// import slots (GetState, SetState for rumble). The mod points those slots at its own two functions, which ask XInput
// for the physical pad the table assigns to the slot the game asked about: the game's controller id N, and with it
// player N's buttons, sticks, menus and rumble, follow the pad assigned to N. The game's reader, its events and its
// per-slot state are its own; only which device answers a slot changes, as Steam Input itself does. After every
// change the interface's own re-check byte is set (the field its WM_DEVICECHANGE handling writes), so a pad moved
// onto a slot the engine counted empty is polled. Identity until changed, so normal play and the virtual test pad
// see no change. The keyboard stays player 1's.
//
// Also here, on the same poll:
//   - idle, per pad: an idle pad reads "nothing pressed" to the game (an absent one stays absent) and its rumble is off
//     (a player's open window: the game's own release events let go of whatever that pad held);
//   - after its idle or a change, a pad stays idle to the game until it lets go of what it was holding then, so the
//     press that closed a window never reaches the game as an attack or a talk (pad_rules LetGo: a resting trigger or
//     a stuck button never holds a pad for good); every pad's rumble is set off at a change;
//   - the opener, View held for a second (pad_rules::Gate): a shorter View press reaches the game at its let-go,
//     so a tap still opens the map (a tap's length late) and the hold never does;
//   - Start only on a slot a player holds (pad_rules::StartGate; slot 0 always): a pad that plays no one never
//     pauses the game, and its Start, heard by the listener, joins (players/player_window); the rest of such a pad
//     still reaches the game as before;
//   - B, told to the listener as a tap (let go within a second) or a hold (a second down; pad_rules::Hold): a
//     window's close and its leave. B itself is not held back.
// A PlayStation pad the game reads through its own scePad device is not XInput and is not moved (logged at install).
// Console: `pad-table` (the pads of players 1-3 and the spare slot, e.g. `pad-table 2 1 3 4`), `pads-idle`; reading `players.pad-table`.

#include "players/pad_rules.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::pad_table {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The table now: slot N's physical XInput user.
pad_rules::PadTable Table() noexcept;

/// Sets the table (a permutation of 0..3, else refused) and has the engine poll every slot again. Game thread.
bool Set(const pad_rules::PadTable& table, const wchar_t* why) noexcept;

/// The physical pads that read idle to the game (bit N: XInput user N); a pad leaving the set waits for its let-go.
/// Game thread.
void SetIdleUsers(std::uint32_t users, const wchar_t* why) noexcept;

/// Each physical pad's new presses (the buttons down on this poll and up on its last), the opener, and B's tap (at its
/// let-go) and hold, as the game's poll reads them, idle or not, Start included: the player windows' input
/// (players/player_window). Called inside the engine's poll, so the listener only notes them (a table change there
/// would lose its re-check: the poll clears the byte at its end). Game thread.
using Listener = void (*)(int user, std::uint16_t pressed, bool open, bool bTap, bool bHeld) noexcept;
void Listen(Listener listener) noexcept;

/// The slots a player holds (bit N: slot N, the controller id the session's seat N plays; bit 0 always): the others'
/// Start is held back from the game. Once per engine tick from the session's players. Game thread.
void SetPlaying(std::uint32_t slots) noexcept;

/// The engine polls a slot it counts empty only after a device change it heard (RE-pad-slots.md §2.1). A pad that
/// comes back, or moves to another XInput number (Steam Input renumbers its pads when one turns off or on), can land
/// on such a slot between those messages and stay dead to the game until the next device change. Given the XInput
/// users that answer (bit N: user N, players/pad_presence's poll, once a second), every slot whose pad answers while
/// the engine counts the slot empty gets the engine's own re-check, the flag its WM_DEVICECHANGE handling sets. Logged
/// when the set of such slots changes. Game thread.
void RecheckAnswering(std::uint32_t answeringUsers) noexcept;

}  // namespace lss::players::pad_table
