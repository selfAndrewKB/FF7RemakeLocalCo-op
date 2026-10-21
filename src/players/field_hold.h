#pragma once

// When a party member's field AI state leaves it a further player's to hold, and when a script needs it back.
//
// The field AI (AEndFieldAIController) is a stack of 15 request layers; the highest non-empty layer is active and
// its kind byte (the reflected FFieldAiType) says what runs. Layer 0 is the stream's default, layer 1 the level's AI
// settings, written during gameplay to stage the members (a stay, a run to a mark, a lead, the follow); layers 2 to
// 12 are the scripts' (acts, events, field actions, story moves) and 13 the cinema copy a cutscene's preparation
// makes of the active record. The user's rule (2026-10-20; research/RE-fieldai-kinds.md): players 2 and 3 keep
// control through the level's staging, and wait, the member handed to its AI, only when a script needs it: an
// animation, the unread ESCAPE_POSITION, or any request on a script layer. A request written on a player-held
// member runs its enter and one update and freezes until the hand; only SINGLE_ANIMATION's exit reports finished
// to the scripts, so nothing else stalls while held.
// ANIMATION on the level layer is a scripted animation too (the user, 2026-10-21, after player 2 seated on Tifa in
// the middle of one and cut it short): both animation kinds hand the member to its AI with the player's view kept
// on it, until the animation ends.
// LEAD on the level layer is the one kind this rule cannot settle (the user, 2026-10-21): the level writes it as
// often to walk a member to a mark in step with the player ("walk up to the console", twice on the Ch. 7 reactor
// walk) as to make the member a guide the player follows ("follow Tifa", Ch. 3). The handler is the same code either
// way; what differs is level data: whether a level step waits on the lead's arrival, and whether the story marks
// the member. A guide on a held member stalls the level (its runner never ticks, the arrival never fires: seen live
// 2026-10-21), so the rule answers Lead and the caller asks the game's own data (players/lead_gate.h): a guide goes
// to its AI with the player's view on it, staging stays the player's.
//
// Pure (host-tested in tests/field_hold_test.cpp); the actor modes and the event lock are the caller's tests.

#include <cstdint>

namespace lss::players::field_hold {

/// Layers of the request stack (fa+0xDF8 names the active one; 16 = no state yet).
inline constexpr std::uint8_t kLayerStream = 0;
inline constexpr std::uint8_t kLayerLevel  = 1;
inline constexpr std::uint8_t kLayerCinema = 13;
inline constexpr std::uint8_t kNoLayer     = 16;

/// FFieldAiType, the kind byte of a record (fa+0xE00+layer*0x4C8) and of the active request (fa+0x55B8).
enum Kind : std::uint8_t
{
    kEmpty           = 0,
    kNone            = 1,   // stand (the "Stable" data setting)
    kStay            = 2,
    kFollowTarget    = 3,   // follow another actor ("FollowActor")
    kFollowPosition  = 4,   // the run to a mark ("MoveToTarget")
    kAnimation       = 5,
    kSingleAnimation = 6,   // the scripts' one-shot move; its exit reports finished
    kLead            = 7,   // walk to a point and wait for the party leader, beckon, fetch: staging or a guide
    kEscapePosition  = 8,   // no writer found
    kWandering       = 9,
    kPatrol          = 10,
    kAccompany       = 11,  // the party follow (the level's default setting)
    kInterest        = 12,
    kUnique          = 13,  // the base controller's route move
};

enum class Hold : std::uint8_t
{
    Free,       // the player's: the level's staging or the follow
    Animation,  // a scripted animation (ANIMATION, SINGLE_ANIMATION): its AI plays it, the player watches
    Layer,      // a script layer is active
    Kind,       // a kind that needs the AI: the unread ESCAPE_POSITION
    Lead,       // a LEAD on the level layer: staging or a guide, the game's data decides (players/lead_gate.h)
    NoState,    // the field AI has no state yet (wait)
};

/// The hold of a field AI state: its active layer and that layer's kind.
constexpr Hold Of(std::uint8_t layer, std::uint8_t kind) noexcept
{
    if (layer >= kNoLayer)
    {
        return Hold::NoState;
    }
    if (layer > kLayerLevel && layer != kLayerCinema)
    {
        return Hold::Layer;
    }
    if (kind == kAnimation || kind == kSingleAnimation)
    {
        return Hold::Animation;
    }
    if (kind == kEscapePosition)
    {
        return Hold::Kind;
    }
    if (kind == kLead && layer == kLayerLevel)
    {
        return Hold::Lead;
    }
    return Hold::Free;
}

constexpr const wchar_t* KindName(std::uint8_t kind) noexcept
{
    switch (kind)
    {
    case kEmpty:
        return L"EMPTY";
    case kNone:
        return L"NONE";
    case kStay:
        return L"STAY";
    case kFollowTarget:
        return L"FOLLOW_TARGET";
    case kFollowPosition:
        return L"FOLLOW_POSITION";
    case kAnimation:
        return L"ANIMATION";
    case kSingleAnimation:
        return L"SINGLE_ANIMATION";
    case kLead:
        return L"LEAD";
    case kEscapePosition:
        return L"ESCAPE_POSITION";
    case kWandering:
        return L"WANDERING";
    case kPatrol:
        return L"PATROL";
    case kAccompany:
        return L"ACCOMPANY";
    case kInterest:
        return L"INTEREST";
    case kUnique:
        return L"UNIQUE";
    }
    return L"?";
}

}  // namespace lss::players::field_hold
