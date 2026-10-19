#pragma once

// Which deployed party member a further player plays, and when it is the player's to take (the user's rule,
// 2026-10-01, carried from Rebirth's player_two/party_member; RE-slot-member.md).
//
// Player 2 plays the member in the 2nd slot of the game's own party order: the row after the field leader in the
// deployed formation (PartyManager+0x230..+0x232, the deployed types in ascending order, the order the party panel
// shows and the battle switch cycles), stepping past player 1's character and the battle-support status as that
// cycle does; a third player would take the next row. The game's order is the only control: a story party change
// moves the slot and the session follows it. The row's member is taken only while it is free the way the game
// hands members to players: nobody else's controller holds it; in the field its AI runs the party's own follow
// (the stream's Stable layer or the level's default AI setting) with no script layer above, it is in the FIELD or
// CAUTION actor mode and no event holds
// the field; in battle the BATTLE mode. A story script that takes a held member (a higher AI layer, the cinema
// modes) gets it back, and the player waits for it. A downed member stays playable, as player 1 stays on a downed
// leader. With fewer rows than seats (a solo story party) that player has no member and waits.
//
// Seat-indexed (seat 1 = player 2); game thread only; every read guarded (a reload tears the party down under them).

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::players::party_member {

bool Install(const platform::GameImage& image) noexcept;

enum class Wait : std::uint8_t
{
    None,        // `member` is the one to take
    NoSlot,      // the formation has no row for this seat beyond player 1's
    NotLoaded,   // the row names a type with no actor
    Held,        // another controller holds the row's member (its PLAYER slot)
    Cinema,      // in a cutscene or an event (the cinema actor modes)
    EventLock,   // an event holds the field
    AiHeld,      // a script holds its field AI (a layer above the party's default, or another kind)
    NoFieldAi,   // its field AI has no state yet
    Unread,      // the party could not be read
};

struct Choice
{
    void*        member  = nullptr;
    std::uint8_t type    = 9;   // the row's player type (facts::kPlayerTypeNone with no row)
    std::int32_t ordinal = -1;  // its ordinal in the formation
    Wait         wait    = Wait::NoSlot;
};

/// The member seat `seat` plays now, or why it waits. `taker` is that seat's controller (a member it already
/// holds counts as free). Logged at Debug when `log` is set.
Choice Choose(int seat, void* playerOneController, void* taker, bool log) noexcept;

/// The actor in seat `seat`'s row whatever its state (null without a row or an actor).
void* RowActor(int seat, void* playerOneController) noexcept;

/// Every formation member's type, PLAYER slot holder and AI detail, one line each (the `party.ai` reading).
std::wstring FormationReading(void* playerOneController) noexcept;

/// Why `member` is no longer seat `seat`'s row to play: the row names another type, the member left the deployed
/// formation, or it became player 1's. Null while it still is. `type` is the member's type as taken.
const wchar_t* WhyNotInSlot(int seat, void* member, std::uint8_t type, void* playerOneController) noexcept;

/// Why a held `member` must go back to its AI now: a cutscene or event mode, or a script's AI layer. Null while
/// it may stay. (The battle mode holds nothing: the battle AI never runs a player-held member.)
const wchar_t* WhyHeldByScript(void* member) noexcept;

/// The member's party status is in the formation (PARTY or battle support).
bool InFormation(std::uint8_t type) noexcept;

/// An event holds the field (the control lock's event bit).
bool EventLocked() noexcept;

/// The game's own hand of a player-held member to its AI (the PLAYER slot cleared, the controller reconciled), and
/// the return to `controller`: what it does for its leader at an event's start and end, done for this member.
bool HandToAi(void* member) noexcept;
bool ReturnTo(void* member, void* controller) noexcept;
/// The member's PLAYER slot is empty (handed to its AI).
bool HandedToAi(void* member) noexcept;

/// The actor type PLAYER set again with the game's setter (the formation rebuild types every member PARTY).
bool ReassertPlayerType(void* member) noexcept;

const wchar_t* Describe(Wait wait) noexcept;

/// The member's actor mode, its field AI's active layer and kind, and the kind byte of each of the 15 layers
/// (for the log: what the take test saw).
std::wstring AiDetail(void* member) noexcept;

}  // namespace lss::players::party_member
