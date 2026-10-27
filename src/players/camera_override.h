#pragma once

// A seat's member's camera overrides reach that seat's camera (RE-lock-camera-ledger.md row X3).
//
// A character's attack animations carry camera-override notify states. Their begin and end call the delegate the
// character holds (a TFunction whose handler is one static function of the game): it looks up "the" operator's
// NotifyOverride flavor through the field root's operator weak, so the root operator plays it. A seat's operator
// binds that delegate on the seat's member with the game's own SetOwnerChara, but the handler still reaches player
// 1's operator: player 2's attacks shook player 1's camera and never player 2's. The game has no per-operator route
// (in single player the one bound character is the leader, and the root operator is its operator). Rebirth met the
// same leak as one global module shared by every camera manager and gave each its own instance; here each operator
// already owns its flavor, only the routing is global.
//
// So the lever is the routing: the handler pointer in the seat's member's delegate is replaced by one that opens
// that seat's operator scope (the root weak answering the seat's operator) and calls the game's handler, so the
// same flavor code runs on the seat's operator with the member's own parameters. The handler is called with the
// character's parameters and no identity, so each seat has a routing function of its own. Written after each
// SetOwnerChara on the seat's operator (the game rewrites the delegate there), checked each operator tick, put back
// when the seat's operator lets the member go. Game thread only (the notifies run in the anim tick on it).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::camera_override {

/// Checks the game's handler; registers the reading. False (logged) if the handler differs.
bool Install(const platform::GameImage& image) noexcept;
/// The game's handler back into every routed character (unload).
void Remove() noexcept;

/// `character` is the owner of the seat's operator now: its delegate's handler becomes the seat's routing one when
/// the game's is there (idempotent; the seat's previous character is given the game's handler back). Null releases.
void Route(int seat, void* character) noexcept;

}  // namespace lss::players::camera_override
