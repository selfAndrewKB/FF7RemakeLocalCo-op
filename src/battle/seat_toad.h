#pragma once

// Player 2's or 3's character turned into a toad, and back, with that player's own controller and camera on it
// (research/RE-toad-seat.md).
//
// The Frog status hands the swap to the battle's toad manager, which on its next battle tick copies the status and
// gauges onto a toad actor, fades the character out and the toad in, and swaps the type's party slot. Only for the
// party leader does the swap move a controller (the game's leader setter, onto the toad and back). Left alone, player
// 2's or 3's controller and camera stay on the hidden character while the toad walks off on its AI.
//
// The manager's apply and cure for a member a seat plays run inside that seat's scopes (its controller as the primary,
// its member as the leader, its command controller in the UI slot), so the game's own leader branch moves the seat's
// controller and its camera onto the toad and back, with the game's fades and the camera's own transition. Right after
// the swap the listener (the session) adopts the character the game moved the controller onto, as after the seat's own
// switch: the leader scope then holds the toad, which the cure's leader branch looks for, even in the same drain.
// Player 1's swaps are the game's; so is a swap while the seat's member is with its AI (its controller is not on it).
//
// Test action `toad <player>` (in a fight): the game's own status setter gives that player's character the Frog status
// for 30 s, through the whole game path (the next battle tick applies it; the expiry cures it).
// Logged: Info for each swap run in a seat's place, whether it landed, and when a seat's scope cannot open; Debug for a
// swap met inside another player's scope, and, once a second for 6 s after each swap, the seat's camera: whether its
// operator owns the character, ticks and has ended the leader-change move, its state, and the view's distance to it (one
// watch at a time: a later swap, any seat's, takes it over).
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::seat_toad {

/// Resolves the functions and registers the test action. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, on the game thread: places the two entry redirects once (from outside the functions).
void Tick() noexcept;

/// The seat whose toad swap runs now (its controller is the one the game's leader setter moves), else 0.
int ToadSeat() noexcept;

/// Called on the game thread right after a swap run in `seat`'s place moved its controller onto `character`.
using SwappedFn = void (*)(void* context, int seat, void* character) noexcept;
/// The listener (one; set before the first battle).
void SetListener(SwappedFn swapped, void* context) noexcept;

}  // namespace lss::battle::seat_toad
