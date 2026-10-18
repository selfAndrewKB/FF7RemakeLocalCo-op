#pragma once

// PROBE: player 2 takes Aerith through the game's own leader funnel.
//
// Every leader change in the game ends in EndPlayerController::SwitchLeaderInternal(controller, character),
// which writes the controller's LeaderCharacter, sets the character's actor type, attaches the controller's
// PlayerTrigger to it and reconciles possession (the PLAYER controller slot wins over the character's AI).
// Field movement is per controller: each End controller's own tick moves its own LeaderCharacter. So player
// 2's controller, given the game's party setup (InitPartyLevel) and then SwitchLeaderInternal(PC2, Aerith),
// possesses her and moves her with no extra driver (Remake/Documentation/research/RE-party-possession.md,
// RE-impl-facts-v0.2.md §7).
//
// The game has one PlayerTrigger (a placed actor), and SwitchLeaderInternal needs one. Whether player 2 gets a
// copy of it or borrows player 1's is the user's decision (Documentation/plan/topics/session.md). This probe BORROWS player
// 1's: while player 2 holds Aerith, player 1's trigger rides on her (player 1's field prompts follow her).
// Unseating gives Aerith back to her AI and re-seats player 1's leader through the same funnel, which puts the
// trigger back on it.

namespace lss::platform {
class GameImage;
}

namespace lss::players::party_seat {

bool Install(const platform::GameImage& image) noexcept;

/// Game thread: seats Aerith on player 2's controller. False (logged) if she is not loaded or the controller
/// cannot take her.
bool Seat(void* playerOneController, void* playerTwoController) noexcept;

/// Game thread: gives player 2's character back to its AI and re-seats player 1's leader. True once player 2's
/// controller no longer holds a character (also when nothing was seated); false (logged) if it still does.
bool Unseat(void* playerOneController, void* playerTwoController) noexcept;

/// Game thread: player 2's controller is gone; a later player 2 is set up afresh.
void Forget() noexcept;

bool Seated() noexcept;

}  // namespace lss::players::party_seat
