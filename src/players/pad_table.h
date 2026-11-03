#pragma once

// Which physical pad answers each of the game's pad slots (plan/topics/input.md State "Planned"; the user's design of
// 2026-10-30 18:43; research/RE-pad-slots.md).
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
//   - idle: every connected pad reads "nothing pressed" (an absent one stays absent) and every pad's rumble is off
//     (the claim screen's state: the game's own release events let go of whatever was held);
//   - after idle or a change, a pad stays idle to the game until it lets go of what it was holding then, so the press
//     that ended a claim never reaches the game as an attack or a talk (pad_claim_rules LetGo: a resting trigger or a
//     stuck button never holds a pad for good); every pad's rumble is set off at a change;
//   - the opener, View held for a second (pad_claim_rules::Gate): a shorter View press reaches the game at its let-go,
//     so a tap still opens the map (a tap's length late) and the hold never does;
//   - Start only on a slot a player holds (pad_claim_rules::StartGate; slot 0 always): a pad that plays no one never
//     pauses the game, and its Start, heard by the listener, joins (players/pad_claim); the rest of such a pad still
//     reaches the game as before;
//   - B, told to the listener as a tap (let go within a second) or a hold (a second down; pad_claim_rules::Hold):
//     the claim's Back and its leave. B itself is not held back.
// A PlayStation pad the game reads through its own scePad device is not XInput and is not moved (logged at install).
// Console: `pad-table` (the pads of players 1-3 and the spare slot, e.g. `pad-table 2 1 3 4`), `pads-idle`; reading `players.pad-table`.

#include "players/pad_claim_rules.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::pad_table {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The table now: slot N's physical XInput user.
pad_claim_rules::PadTable Table() noexcept;

/// Sets the table (a permutation of 0..3, else refused) and has the engine poll every slot again. Game thread.
bool Set(const pad_claim_rules::PadTable& table, const wchar_t* why) noexcept;

/// Every connected pad reads idle to the game while on. Game thread.
void SetIdle(bool on, const wchar_t* why) noexcept;

/// Each physical pad's new presses (the buttons down on this poll and up on its last), the opener, and B's tap (at its
/// let-go) and hold, as the game's poll reads them, idle or not, Start included: the claim's input (players/pad_claim). Called
/// inside the engine's poll, so the listener only notes them (a table change there would lose its re-check: the poll
/// clears the byte at its end). Game thread.
using Listener = void (*)(int user, std::uint16_t pressed, bool open, bool bTap, bool bHeld) noexcept;
void Listen(Listener listener) noexcept;

/// The slots a player holds (bit N: slot N, the controller id the session's seat N plays; bit 0 always): the others'
/// Start is held back from the game. Once per engine tick from the session's players. Game thread.
void SetPlaying(std::uint32_t slots) noexcept;

/// Logs the engine's other input devices (a PlayStation pad read through scePad is not moved by the table). Game thread.
void LogDevices() noexcept;

}  // namespace lss::players::pad_table
