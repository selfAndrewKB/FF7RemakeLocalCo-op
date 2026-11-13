#pragma once

// Which deployed party member a further player plays, and when it is the player's to take
// (research/RE-slot-member.md).
//
// The first further player plays the member in the 2nd slot of the game's own party order: the row after the field
// leader in the deployed formation (PartyManager+0x230..+0x232, the deployed types in ascending order, the order the
// party panel shows and the battle switch cycles), stepping past player 1's character and the battle-support status
// as that cycle does; the next player takes the next row. Each further player's place among those rows is its rank
// (players/party_rank_rules.h): a join takes the lowest rank no other player holds, and a player window's arrows step it
// (players/player_window). The game's order is the only other control: a story party change moves the rows and the
// session follows it. The row's member is taken only while it is free the way the game
// hands members to players: nobody else's controller holds it; in the field its AI runs the level's staging or the
// party's follow (players/field_hold.h: no script layer active, no animation or one-shot kind), it is in the FIELD
// or CAUTION actor mode, no push-button event holds the field with another lock and the game's own leader is with its
// player (not handed to its AI for a scene, a field action or the control lock), a treasure or push-button lock alone
// excepted (field_hold::InteractionLock: the member is then judged by its own state); in battle the BATTLE mode. A story script that takes a held
// member (a script layer, an animation, the cinema modes) gets it the way the game hands its leader to its AI, the
// seat and the player's view kept on it, until the hold ends. A LEAD the level writes on
// the member is the player's while it is staging or kept (its point marked), and the AI's while it is a guide or once
// the player brings it to its point (the game's own data decides: players/lead_gate.h). A downed member stays playable, as player 1 stays on a downed leader. With fewer rows than
// seats (a solo story party) that player has no member and waits.
//
// Seat-indexed (seat 1 = player 2); game thread only; every read guarded (a reload tears the party down under them).

#include "players/field_hold.h"

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
    Support,     // the rows beyond player 1's hold only battle-support members (PARTY_Battle_Support: guests the game
                 // fights with and skips in its own switch cycle; Barret before the opening's switch tutorial ends)
    NotLoaded,   // the row names a type with no actor
    Held,        // another controller holds the row's member (its PLAYER slot)
    Cinema,       // in a cutscene or an event (the cinema actor modes)
    PushButtonEvent, // a push-button event (a switch, a lever, a terminal) holds the field with another lock
    LeaderHanded, // the game's own leader is with its AI (a scene, a field action, the control lock), or unread
    AiHeld,       // a script holds its field AI (a script layer, or the unread ESCAPE_POSITION kind: field_hold.h)
    Animation,    // its AI plays a scripted animation (ANIMATION or the one-shot SINGLE_ANIMATION: field_hold.h)
    Guide,        // its AI walks a guide: a lead or a run to a mark the level waits on, or the story marks it (lead_gate.h)
    AtMark,       // its player brought a kept move to its point, or to a spot on the way only its AI may take: its AI
                  // finishes the move (lead_keep_rules.h)
    SceneLeadIn,  // the level's scene lead-in on it, a run and a stand a scene opens on: its AI plays it through, whatever
                  // the lead rule would say (lead_keep_rules.h kSceneLeadIns, lead_gate.h)
    FieldAction,  // no player holds it and it is inside a field action (a ladder, a squeeze, a crawl, a step) its AI began:
                  // a take now would run the rest of the action as the player's and end it where it stands
    NoFieldAi,    // its field AI has no state yet
    Unread,       // the party could not be read
};

/// The waits a seated member's own scripted hold makes: a cutscene mode, a script layer or kind, an animation, a
/// guide lead, a kept move brought to its point, a scene lead-in. The session answers each with the game's own hand (the
/// member to its AI, the seat and the player's view kept) and takes the member back when the hold ends. The rest (a
/// push-button event, the game's leader handed) is the game's own hand of its leader, heard by party_events.
constexpr bool ScriptHold(Wait wait) noexcept
{
    return wait == Wait::Cinema || wait == Wait::AiHeld || wait == Wait::Animation || wait == Wait::Guide || wait == Wait::AtMark ||
           wait == Wait::SceneLeadIn;
}

/// The holds a joining player takes the member through, as a seated player rides them: a guide, a kept move at its
/// point or a scene lead-in, each a walk the member's AI makes. A walk survives the take: a request on a member a player
/// holds freezes, and handed back it starts again from where the member stands; only a one-shot animation reports itself
/// finished when taken (RE-fieldai-kinds.md, "Possession"). So the join seats the member and hands it straight back to
/// its AI (the seat and the player's view kept), and the controller takes it when the walk ends. The other holds (a
/// cutscene mode, a script layer, an animation) still wait: taking the member could end the script's step.
constexpr bool JoinsThroughHold(Wait wait) noexcept
{
    return wait == Wait::Guide || wait == Wait::AtMark || wait == Wait::SceneLeadIn;
}

/// A take may go ahead on this wait: none, or a walk the join rides through.
constexpr bool Takeable(Wait wait) noexcept
{
    return wait == Wait::None || JoinsThroughHold(wait);
}

/// The walks the member's AI makes under a hold the lead gate judged: their judgment goes with the hold
/// (lead_gate::Forget). The same holds a join rides through, since each is such a walk.
constexpr bool GateWalk(Wait wait) noexcept
{
    return JoinsThroughHold(wait);
}

struct Choice
{
    void*        member  = nullptr;  // the one to take: free (wait None), or in a walk a join takes it through (wait names it)
    std::uint8_t type    = 9;   // the row's player type (facts::kPlayerTypeNone with no row)
    std::int32_t ordinal = -1;  // its ordinal in the formation
    Wait         wait    = Wait::NoSlot;
    std::uint32_t formation = 0;  // the formation as read: types[0] | types[1] << 8 | types[2] << 16 | field leader << 24; 0 unread
};

/// The member seat `seat` plays now, or why it waits: the row at the seat's rank (Rank). A member in a walk its AI makes
/// (JoinsThroughHold) is given with the walk as its wait; the session hands it back to its AI once seated. `taker` is
/// that seat's controller (a member it already holds counts as free). `standIn`: the type of player 1's former character the seat
/// stands in for since the game's leader change took its member (facts::kPlayerTypeNone: none); it is the seat's while
/// deployed and not player 1's, whatever the rank. Logged at Debug when `log` is set.
Choice Choose(int seat, void* playerOneController, void* taker, bool log, std::uint8_t standIn) noexcept;

/// Choose at rank `rank` in place of the seat's own: the member the seat would play there now, or why it would wait
/// (a join's pick and a player window's step test a rank before taking it).
Choice ChooseAt(int seat, int rank, void* playerOneController, void* taker, bool log, std::uint8_t standIn) noexcept;

/// The seat's rank: its place among the rows a further player may play, counted from the field leader's in the
/// formation's order (players/party_rank_rules.h; 1 the first). The seat's own number until the session sets it. A
/// changed rank makes the seat's member no longer its row (WhyNotInSlot), so the session gives it back and seats the
/// new row's. Game thread.
int  Rank(int seat) noexcept;
void SetRank(int seat, int rank) noexcept;
/// How many ranks the party has now: the rows past the field leader that are neither battle support nor player 1's
/// (player 1's counts while player 1 is away from the field leader, as Choose counts it). 0 when unread.
int Ranks(void* playerOneController) noexcept;

/// The actor in seat `seat`'s row whatever its state (null without a row or an actor).
void* RowActor(int seat, void* playerOneController, std::uint8_t standIn) noexcept;

/// Every formation member's type, PLAYER slot holder and AI detail, one line each (the `party.ai` reading).
std::wstring FormationReading(void* playerOneController) noexcept;

/// Why `member` is no longer seat `seat`'s row to play: the row names another type, the member left the deployed
/// formation, or it became player 1's. Null while it still is. `type` is the member's type as taken.
const wchar_t* WhyNotInSlot(int seat, void* member, std::uint8_t type, void* playerOneController, std::uint8_t standIn) noexcept;

/// The field leader's type (the default party leader, player 1's home character) and the game's leader type now
/// (PM+0x233, player 1's character outside every scope); facts::kPlayerTypeNone when unread.
std::uint8_t FieldLeaderType() noexcept;
std::uint8_t GameLeaderType() noexcept;
/// `character` is the field leader's actor (the game's leader returning to it is player 1 going home).
bool IsFieldLeader(const void* character) noexcept;

/// Why a held `member` must be with its AI now (a ScriptHold wait: a cutscene or event mode, a script's hold on its
/// field AI, an animation, a guide the level waits on). Null while it may stay. (The battle mode holds nothing: the
/// battle AI never runs a player-held member; a push-button event or the game's leader handed is the game's own hand, heard
/// by party_events.)
const wchar_t* WhyHeldByScript(void* member) noexcept;

/// The field take test's wait for a `member` whoever holds it (None: free, or in battle), the reason itself: the
/// session hands a ScriptHold wait's member to its AI keeping its seat, and takes it back at None.
Wait HoldWait(void* member) noexcept;

/// The character's field AI controller (facts::kCharacterFieldAi), or null. Any thread: one guarded read.
std::uint8_t* FieldAiController(const void* member) noexcept;

/// The layer and kind `member`'s field AI hold is judged by: its active request, or, for an interest scene while
/// players/ai_move_guard is active, the one beneath it (field_hold::JudgedState: an interest scene never takes a
/// member). False when unread.
bool JudgedState(const void* member, field_hold::Judged& out) noexcept;

/// The member's active request is an INTEREST (an ambient interest walk). A hold judged beneath it still hands the
/// member to its AI, which then runs the walk first, as in single player: the session says so in its hand line.
bool InterestOnTop(const void* member) noexcept;

/// The member's actor mode byte (facts::kActorMode*; 0 unread).
std::uint8_t ActorMode(const void* member) noexcept;

/// The member's party status is in the formation (PARTY or battle support).
bool InFormation(std::uint8_t type) noexcept;

/// The player type of a deployed `member` (the formation's row naming its actor); facts::kPlayerTypeNone when it is in no
/// row or the party cannot be read.
std::uint8_t TypeOf(void* member) noexcept;

/// The field control lock mask (0 when free or unread), and whether its push-button event bit is set.
std::uint32_t ControlLockMask() noexcept;
bool          PushButtonLocked() noexcept;

/// The talk that took the field lock is player 1's own prompt talk and the `talk-follow` option leaves the other
/// players' characters with their players (world/talk_gather judges it at the game's hand of its leader; the session
/// sets it there and clears it at the leader's return). While set, the talk's lock bit holds no member by itself.
void SetTalkKept(bool kept) noexcept;
bool TalkKept() noexcept;
/// Whether the lock `mask` leaves a further player's member with its player (field_hold::KeepsMembers): a treasure or
/// push-button lock, or the talk's lock while the talk is kept. The member is then judged by its own state.
bool LockKeepsMembers(std::uint32_t mask) noexcept;

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
