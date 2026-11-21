#pragma once

// Which physical pad answers each of the game's pad slots (plan/topics/input.md; research/RE-pad-slots.md,
// RE-playstation-pads.md).
//
// The game reads XInput pads in one place, Unreal's stock XInputInterface::SendControllerEvents, through xinput1_3's
// two import slots (GetState, SetState for rumble). The mod points those slots at its own two functions, which ask
// XInput for the physical pad the table assigns to the slot the game asked about: the game's controller id N, and with
// it player N's buttons, sticks, menus and rumble, follow the pad assigned to N. The game's reader, its events and its
// per-slot state are its own; only which device answers a slot changes, as Steam Input itself does. After every
// change the interface's own re-check byte is set (the field its WM_DEVICECHANGE handling writes), so a pad moved
// onto a slot the engine counted empty is polled. Identity until changed, so normal play sees no change. The keyboard
// plays beside player 1's pad, or one player's alone as a slot's live "reader" (pad_rules::KeyboardSlot).
//
// Also here, on the same poll:
//   - idle, per pad: an idle pad reads "nothing pressed" to the game (an absent one stays absent) and its rumble is off
//     (a player's open window: the game's own release events let go of whatever that pad held);
//   - after its idle or a change, a pad stays idle to the game until it lets go of what it was holding then, so the
//     press that closed a window never reaches the game as an attack or a talk (pad_rules LetGo: a resting trigger or
//     a stuck button never holds a pad for good); every pad's rumble is set off at a change;
//   - the opener, View (Create on a PlayStation pad) held for a second (pad_rules::Gate): a shorter View press reaches
//     the game at its let-go, so a tap still opens the map (a tap's length late) and the hold never does;
//   - Start only on a slot a player holds (pad_rules::StartGate; slot 0 always): a pad that plays no one never
//     pauses the game, and its Start, heard by the listener, joins (players/player_window); the rest of such a pad
//     still reaches the game as before;
//   - B, told to the listener as a tap (let go within a second) or a hold (a second down; pad_rules::Hold): a
//     window's close and its leave. B itself is not held back.
//
// PlayStation pads: the game reads them itself, through Sony's scePad library linked into the exe, one record per slot,
// each record holding a pad's scePad handle (research/RE-playstation-pads.md). The mod takes that reader's one read
// call: the same rules run on its pads (Options is Start, Create View, Circle B; the library's system-button switch,
// held on while the mod runs, lets Create through, and the read takes Create, PS and Mute out of the game's data again,
// so the game sees what it always did, the touchpad as its map button), and a slot's PlayStation pad is moved by
// writing the handle the game's record holds, then clearing the record's connected byte so the game's own connect path
// runs for the pad that now has the slot. Its rumble call is taken too, for an idle pad's. So each slot has two pads,
// one per reader; the one that plays the slot's player is the slot's live reader's, and the other is muted while the
// live one is present (pad_rules::Muted). Pads are numbered 0..3 for XInput's users and 4..7 for the PlayStation pads
// (scePad users 1..4).
// Console: `pad-table` (the pads of players 1-3 and the spare slot, e.g. `pad-table 2 1 3 4`, or `pad-table p1 1 3 4`
// for the first PlayStation pad on player 1), `pads-idle`; reading `players.pad-table`.

#include "players/pad_rules.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::pad_table {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The table now.
pad_rules::PadTable Table() noexcept;

/// Sets the table (pad_rules::IsValid, else refused) and has the engine poll every slot again; a PlayStation lane's
/// change reaches its record at the record's next read. Game thread.
bool Set(const pad_rules::PadTable& table, const wchar_t* why) noexcept;

/// The physical pads the player windows hold idle to the game (bit p: pad p); a pad leaving the set waits for its
/// let-go. The muted pads read idle too. Game thread.
void SetIdlePads(std::uint32_t pads, const wchar_t* why) noexcept;

/// The pads that answered their last read (bit p: pad p). Game thread.
std::uint32_t PresentPads() noexcept;

/// The PlayStation pad the reader's record for `slot` holds is a DualShock 4 (else a DualSense, or no PlayStation pad):
/// the library's own answer for that pad, asked once per pad the record holds. Game thread.
bool DualShock4(int slot) noexcept;

/// The controller id the keyboard and mouse play on: the slot they play alone while a player holds it (SetPlaying),
/// else 0, player 1's (beside player 1's pad, alone on player 1's slot, or on a slot whose player is not in yet).
int KeyboardUser() noexcept;

/// Each physical pad's new presses as XInput bits (the buttons down on this poll and up on its last), the opener, and
/// B's tap (at its let-go) and hold, as the game's poll reads them, idle or not, Start included: the player windows'
/// input (players/player_window). Called inside the engine's poll, so the listener only notes them (a table change
/// there would lose its re-check: the poll clears the byte at its end). Game thread.
using Listener = void (*)(int pad, std::uint16_t pressed, bool open, bool bTap, bool bHeld) noexcept;
void Listen(Listener listener) noexcept;

/// The other reader's pad that pressed `button` (one XInput bit) within a poll's reach of `pad`'s own last press of
/// it, by the polls' clocks: one pad read by both readers (Steam Input shows a PlayStation pad as an XInput pad while
/// the game also reads it itself). -1 for none. Game thread.
int TwinOf(int pad, std::uint16_t button) noexcept;

/// The slots a player holds (bit N: slot N, the controller id the session's seat N plays; bit 0 always): the others'
/// Start is held back from the game, and the muted pads follow from it. Once per engine tick from the session's
/// players. Game thread.
void SetPlaying(std::uint32_t slots) noexcept;

/// The engine polls a slot it counts empty only after a device change it heard (research/RE-pad-slots.md). A pad that
/// comes back, or moves to another XInput number (Steam Input renumbers its pads when one turns off or on), can land
/// on such a slot between those messages and stay dead to the game until the next device change. Given the XInput
/// users that answer (bit N: user N, players/pad_presence's poll, once a second), every slot whose pad answers while
/// the engine counts the slot empty gets the engine's own re-check, the flag its WM_DEVICECHANGE handling sets. Logged
/// when the set of such slots changes. Game thread.
void RecheckAnswering(std::uint32_t answeringUsers) noexcept;

}  // namespace lss::players::pad_table
