#pragma once

// When a party member's field AI state leaves it a further player's to hold, and when a script needs it back.
//
// The field AI (AEndFieldAIController) is a stack of 15 request layers; the highest non-empty layer is active and
// its kind byte (the reflected FFieldAiType) says what runs. Layer 0 is the stream's default, layer 1 the level's AI
// settings, written during gameplay to stage the members (a stay, a run to a mark, a lead, the follow); layers 2 to
// 12 are the scripts' (acts, events, field actions, story moves) and 13 the cinema copy a cutscene's preparation
// makes of the active record. The user's rule (2026-10-20; research/RE-fieldai-kinds.md): players 2 and 3 keep
// control through the level's staging, and wait, the member handed back to its AI, only when a script needs it: a
// LEAD (the member guides the party leader to a point and waits for him: the one layer-1 kind that reads the
// player), a one-shot animation, the unread ESCAPE_POSITION, or any request on a script layer. A request written
// on a player-held member runs its enter and one update and freezes until the give-back; only SINGLE_ANIMATION's
// exit reports finished to the scripts, so nothing else stalls while held.
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
    kLead            = 7,   // guide the party leader to a point, wait, beckon, fetch
    kEscapePosition  = 8,   // no writer found
    kWandering       = 9,
    kPatrol          = 10,
    kAccompany       = 11,  // the party follow (the level's default setting)
    kInterest        = 12,
    kUnique          = 13,  // the base controller's route move
};

enum class Hold : std::uint8_t
{
    Free,     // the player's: the level's staging or the follow
    Layer,    // a script layer is active
    Kind,     // a kind that needs the AI: LEAD, SINGLE_ANIMATION, ESCAPE_POSITION
    NoState,  // the field AI has no state yet (wait)
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
    if (kind == kSingleAnimation || kind == kLead || kind == kEscapePosition)
    {
        return Hold::Kind;
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
