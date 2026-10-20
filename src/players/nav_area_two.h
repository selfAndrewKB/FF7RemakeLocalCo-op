#pragma once

// Player 2's nav-area component ticks in player 2's scopes (RE-operator-settings-two.md §2.5).
//
// Each End player controller owns a nav-area component whose tick diffs its own leader's nav areas and, on an
// InDoor or Town volume's enter and leave, pushes or pops that volume's camera setting on "the" field camera
// operator (the field root's weak), and marks the leader actor and the primary controller. Player 2's controller
// gets the component with the game's own InitPartyLevel, so it ticked with no scope: Aerith's enters landed on
// player 1's operator, her leaves popped the key Cloud's enter had pushed there (player 1's camera lost InDoor
// while Cloud was still inside), and player 2's operator never got InDoor or Town.
// The lever: the component's tick slot runs the game's tick inside player 2's primary, operator and leader scopes
// when the component is player 2's controller's, so the same code reaches player 2's operator, member and
// controller. Game thread only (a component tick).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::nav_area_two {

/// Replaces the component's tick slot; registers the reading `nav-area-two`. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::players::nav_area_two
