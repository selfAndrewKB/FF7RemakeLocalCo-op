#pragma once

// A further player's controller takes a party member through the game's own leader funnel, and gives it back.
//
// Every leader change in the game ends in EndPlayerController::SwitchLeaderInternal(controller, character), which
// writes the controller's LeaderCharacter, sets the character's actor type, attaches the controller's PlayerTrigger
// to it and reconciles possession (the PLAYER controller slot wins over the character's AI). Field movement is per
// controller: each End controller's own tick moves its own LeaderCharacter. So a further player's controller, given
// the game's party setup (InitPartyLevel, once per controller) and then SwitchLeaderInternal(PC, member), possesses
// the member and moves it with no extra driver (research/RE-party-possession.md; RE-impl-facts-v0.2.md). Which
// member: players/party_member.h. The release is the same funnel with a null character.
//
// The game has one PlayerTrigger (a placed actor) and SwitchLeaderInternal needs one per controller: each further
// player gets its own copy (players/player_trigger.h); only if none can be made is player 1's borrowed, which then
// rides on the member (encounters then follow it) and is put back at the release.
// Seat-indexed (seat 1 = player 2); game thread only.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::party_seat {

bool Install(const platform::GameImage& image) noexcept;

/// Seats `member` (of player type `type`) on seat `seat`'s controller. False (logged) if the controller cannot
/// take it, or it is player 1's leader.
bool SeatMember(int seat, void* playerOneController, void* controller, void* member, std::uint8_t type) noexcept;

/// Gives the seat's member back to its AI (and re-seats player 1's leader if its trigger was borrowed). True once
/// the controller no longer holds a character (also when nothing was seated); false (logged) if it still does.
bool Unseat(int seat, void* playerOneController, void* controller) noexcept;

/// The seat's controller is gone; a later one is set up afresh.
void Forget(int seat) noexcept;

bool         Seated(int seat) noexcept;
void*        Member(int seat) noexcept;      // the member seated, or null
std::uint8_t MemberType(int seat) noexcept;  // 9 (NONE) when none

}  // namespace lss::players::party_seat
