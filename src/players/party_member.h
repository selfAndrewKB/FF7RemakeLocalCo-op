#pragma once

// Which deployed party member a further player plays, and when it is the player's to take (the user's rule,
// 2026-10-01, carried from Rebirth's player_two/party_member; RE-slot-member.md).
//
// Player 2 plays the member in the 2nd slot of the game's own party order: the row after the field leader in the
// deployed formation (PartyManager+0x230..+0x232, the deployed types in ascending order, the order the party panel
// shows and the battle switch cycles), stepping past player 1's character and the battle-support status as that
// cycle does; a third player would take the next row. The game's order is the only control: a story party change
// moves the slot and the session follows it. The row's member is taken only while it is free the way the game
// hands members to players: nobody else's controller holds it; in the field its AI runs the level's staging or the
// party's follow (players/field_hold.h: no script layer active, no animation or one-shot kind), it is in the FIELD
// or CAUTION actor mode, no event holds the field and the game's own leader is with its player (not handed to its
// AI for a scene, a field action or the control lock); in battle the BATTLE mode. A story script that takes a held
// member (a script layer, an animation, the cinema modes) gets it the way the game hands its leader to its AI, the
// seat and the player's view kept on it, until the hold ends (the user, 2026-10-21). A LEAD the level writes on
// the member is the player's while it is staging and the AI's while it is a guide (the game's own data decides:
// players/lead_gate.h). A downed member stays playable, as player 1 stays on a downed leader. With fewer rows than
// seats (a solo story party) that player has no member and waits.
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
    Cinema,       // in a cutscene or an event (the cinema actor modes)
    EventLock,    // an event holds the field
    LeaderHanded, // the game's own leader is with its AI (a scene, a field action, the control lock), or unread
    AiHeld,       // a script holds its field AI (a script layer, or the unread ESCAPE_POSITION kind: field_hold.h)
    Animation,    // its AI plays a scripted animation (ANIMATION or the one-shot SINGLE_ANIMATION: field_hold.h)
    Guide,        // its AI walks a guide lead (a level step waits on its arrival, or the story marks it: lead_gate.h)
    NoFieldAi,    // its field AI has no state yet
    Unread,       // the party could not be read
};

/// The waits a seated member's own scripted hold makes: a cutscene mode, a script layer or kind, an animation, a
/// guide lead. The session answers each with the game's own hand (the member to its AI, the seat and the player's
/// view kept) and takes the member back when the hold ends. The rest (the event lock, the game's leader handed) is
/// the game's own hand of its leader, heard by party_events.
constexpr bool ScriptHold(Wait wait) noexcept
{
    return wait == Wait::Cinema || wait == Wait::AiHeld || wait == Wait::Animation || wait == Wait::Guide;
}

struct Choice
{
    void*        member  = nullptr;
    std::uint8_t type    = 9;   // the row's player type (facts::kPlayerTypeNone with no row)
    std::int32_t ordinal = -1;  // its ordinal in the formation
    Wait         wait    = Wait::NoSlot;
    std::uint32_t formation = 0;  // the formation as read: types[0] | types[1] << 8 | types[2] << 16 | field leader << 24; 0 unread
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

/// Why a held `member` must be with its AI now (a ScriptHold wait: a cutscene or event mode, a script's hold on its
/// field AI, an animation, a guide lead). Null while it may stay. (The battle mode holds nothing: the battle AI
/// never runs a player-held member; an event lock or the game's leader handed is the game's own hand, heard by
/// party_events.)
const wchar_t* WhyHeldByScript(void* member) noexcept;

/// The field take test's wait for a `member` whoever holds it (None: free, or in battle), the reason itself: the
/// session hands a ScriptHold wait's member to its AI keeping its seat, and takes it back at None.
Wait HoldWait(void* member) noexcept;

/// The member's actor mode byte (facts::kActorMode*; 0 unread).
std::uint8_t ActorMode(const void* member) noexcept;

/// The member's party status is in the formation (PARTY or battle support).
bool InFormation(std::uint8_t type) noexcept;

/// The field control lock mask (0 when free or unread), and whether its event bit is set.
std::uint32_t ControlLockMask() noexcept;
bool          EventLocked() noexcept;

/// The game's own leader is with a player controller (its PLAYER slot filled): the game has not handed it to its
/// AI, or has returned it. False while handed, and when the leader cannot be read.
bool LeaderWithPlayer() noexcept;

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
