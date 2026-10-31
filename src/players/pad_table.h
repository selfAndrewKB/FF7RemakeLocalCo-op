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
//   - the opening combination, View and Menu together (pad_claim_rules::Gate): either button alone reaches the game up
//     to 150 ms late, so the combination never opens the map or the pause menu.
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

}  // namespace lss::players::pad_table
