#pragma once

// Each seat's field camera operator enters and leaves battle mode with player 1's (RE-operator-settings-two.md).
//
// The game's battle camera is a mode of its one field camera operator. One function enters it: the Battle setting
// pushed (the battle row's FOV, distance and height), the field-only SpaceOffset flavor off, TargetLength and
// TargetOffset restarted, the battle length scales and pitch limits selected. An inline block of the field on/off
// table leaves it. Both resolve the operator through the field root's weak, so they reached player 1's operator
// only: player 2's played every battle on the field's Default setting, at half the camera height, with no target
// length and with SpaceOffset moving its aim (Rebirth's camera manager never activated the Battle operator for
// player 2 either, the same class).
// Levers:
//   - the game's enter function runs again inside each seat's operator scope at each of its three call sites (it
//     reads no identity: run on a seat's operator it changes only that operator), and once when a seat's
//     operator is spawned while player 1's is already in battle mode (a join mid-battle);
//   - the leave has no function of its own: when the on/off table pops the Battle request (the block's first
//     call), the same steps run on each seat's operator, the game's own functions where they exist (the pop,
//     SetStateFree, the MoveFollow elapsed reset) and the block's byte and pointer writes mirrored. A recreation:
//     no game path leaves battle mode on a further operator (the RE doc's §4).
// The options menu's Camera Distance (field and battle) reached player 1's operator only, for the same reason: its
// setter writes the root operator's two option bytes. Each seat's operator copies them at its spawn; a change made
// during play is now made on each seat's operator too, the game's setter run again inside its operator scope with
// the menu's own values (RE-camera-distance-option.md).
// Game thread only (the on/off table, the encounter manager and the options menu run there).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::operator_battle_mode {

/// Checks the functions and redirects the enter function's call sites and the battle-end pop; registers the
/// reading `operator-battle-mode`. False (logged) if an engine check fails.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The seat's operator was just spawned and owned (its scope bound): enters battle mode on it now if player 1's
/// operator is in battle mode.
void OnOperatorStarted(int seat, void* seatOperator, void* operatorOne) noexcept;

}  // namespace lss::players::operator_battle_mode
