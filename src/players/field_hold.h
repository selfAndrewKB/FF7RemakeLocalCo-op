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
// LEAD on the level layer is a kind this rule cannot settle (the user, 2026-10-21): the level writes it as often
// to walk a member to a mark in step with the player ("walk up to the console", twice on the Ch. 7 reactor walk) as
// to make the member a guide the player follows ("follow Tifa", Ch. 3). The run to a mark (FOLLOW_POSITION,
// "MoveToTarget") on the level layer is the same case (the user, 2026-10-27: Aerith's run ahead of Cloud in Ch. 8
// stalled the level while a further player held her, until that player left): a run the level merely stages, or
// one a level step waits on. The handler is the same code either way; what differs is level data: whether a level
// step waits on the arrival, and whether the story marks the member. A guide on a held member stalls the level (its
// runner never ticks, the arrival never fires: seen live 2026-10-21 and 2026-10-27), so the rule answers Arrival
// and the caller asks the game's own data (players/lead_gate.h): a guide goes to its AI with the player's view on
// it, staging stays the player's.
//
// Pure (host-tested in tests/field_hold_test.cpp); the actor modes and the push-button event are the caller's tests.

#include <cstddef>
#include <cstdint>
#include <string_view>

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
    kFollowPosition  = 4,   // the run to a mark ("MoveToTarget"); its arrival signals the level as a LEAD's does
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

/// The field control lock bits (EndFieldExclusion, research/RE-script-release.md §3) that freeze only the character
/// that interacts: opening a treasure or picking an item up (_Treasure) and a push-button action (_PushButtonAction:
/// a switch, a lever, a terminal). The game hands only its leader for them, the interactor; an event's other actors
/// are the ones it lists, which it puts in the cinema actor mode and moves on a script layer (both a member's own
/// hold). So under such a lock a further player's member is judged by its own state, and stays its player's when
/// nothing holds it (the user, 2026-10-27: keep moving through a switch, a terminal or a pickup). The game hands only
/// its own leader (player 1's character) for these locks, whichever player interacts; a further player's own chest
/// holds its opener on a script layer of its own (RE-chest-two.md), which the member's own state already answers.
inline constexpr std::uint32_t kLockTreasure     = 0x4;
inline constexpr std::uint32_t kLockPushButton   = 0x100000;
inline constexpr std::uint32_t kInteractionLocks = kLockTreasure | kLockPushButton;

/// Whether the lock `mask` is made only of interaction bits (and is set at all).
constexpr bool InteractionLock(std::uint32_t mask) noexcept
{
    return mask != 0 && (mask & ~kInteractionLocks) == 0;
}

/// The kinds whose arrival signals the level, so a level step can wait on it: a LEAD and the run to a mark
/// (engine_facts.h kArrivalHandlerNames names their handlers' fields; lead_gate.cpp asserts the two lists agree).
/// FOLLOW_TARGET (3) and the route move (13) signal through the same path, PATROL (10) sends its own triggers at
/// its points [CONFIRMED code, research/RE-fieldai-kinds.md §2]; their handlers' names are unread and none has been
/// seen on a party member: free, the same latent stall [UNKNOWN].
constexpr bool SignalsArrival(std::uint8_t kind) noexcept
{
    return kind == kLead || kind == kFollowPosition;
}

/// ASCII letters compared without case, as the game's arrival sender compares a row's Status (its inlined Stricmp).
constexpr bool SameIgnoringCase(std::wstring_view a, std::wstring_view b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const wchar_t x = a[i] >= L'A' && a[i] <= L'Z' ? static_cast<wchar_t>(a[i] + (L'a' - L'A')) : a[i];
        const wchar_t y = b[i] >= L'A' && b[i] <= L'Z' ? static_cast<wchar_t>(b[i] + (L'a' - L'A')) : b[i];
        if (x != y)
        {
            return false;
        }
    }
    return true;
}

/// The level settings that move a member and signal its arrival: the four Status names
/// StateMgr_OnActorSettingArrived_SendArg2Signal 0x14114E100 compares, without case [CONFIRMED code]. One such
/// move that ends in a stand is a placement (Barret's lead path on the Mako 5 save: Lead, then Stable); a further move
/// ahead in the member's own chain before it stands is the level walking the member along a path, which a hold
/// breaks at the first link (Aerith's guide home, Ch. 8, live 2026-10-27: run, stay, run, ... with no Lead setting
/// in it at all; the "follow this character" the user means is built from runs and stays).
constexpr bool MoveSetting(std::wstring_view status) noexcept
{
    return SameIgnoringCase(status, L"Lead") || SameIgnoringCase(status, L"MoveToTarget") ||
           SameIgnoringCase(status, L"FollowActor") || SameIgnoringCase(status, L"RouteMove");
}

/// The Stable setting: the member stands again and its scripted path is over; a move after it is the level's next
/// script (a later beat's), not this path's, so the walk stops here.
constexpr bool PathEnd(std::wstring_view status) noexcept
{
    return SameIgnoringCase(status, L"Stable");
}

enum class Hold : std::uint8_t
{
    Free,       // the player's: the level's staging or the follow
    Animation,  // a scripted animation (ANIMATION, SINGLE_ANIMATION): its AI plays it, the player watches
    Layer,      // a script layer is active
    Kind,       // a kind that needs the AI: the unread ESCAPE_POSITION
    Arrival,    // a LEAD or a run to a mark on the level layer: staging or a guide, the game's data decides
                // (players/lead_gate.h)
    NoState,    // the field AI has no state yet (wait)
};

/// The request layers (fa+0xE00 + layer * 0x4C8, the kind byte first).
inline constexpr std::size_t kLayers = 15;

/// The layer and kind a hold is judged by.
struct Judged
{
    std::uint8_t layer = kNoLayer;
    std::uint8_t kind  = kEmpty;
};

/// The active request, or the one beneath an INTEREST. INTEREST is the companion's ambient walk to, or look at, an
/// interest point the level placed (AEndInterestPoint, e.g. "run aside and watch Cloud cross the monkey bars"; its only
/// writer is the AI model's interest intention, which reacts to any member but the leader inside the point's sphere,
/// in single player too). Nothing waits on it: no arrival signal, no script step. Its AI plays it (the user, 2026-10-29:
/// Aerith should run aside for the monkey bars); a walk that loops under the AI (two crossings) is declined and, with
/// players/ai_move_guard placed (its walk cannot start under a player), judged here: the hold is the next non-empty
/// layer's below it, a lead or a script written meanwhile included (Aerith's 5-minute ladder loop;
/// research/RE-interest-ladder-loop.md). `kinds`: the 15 layers' kind bytes.
constexpr Judged JudgedState(std::uint8_t activeLayer, std::uint8_t activeKind, const std::uint8_t (&kinds)[kLayers]) noexcept
{
    if (activeKind != kInterest || activeLayer >= kLayers)
    {
        return {activeLayer, activeKind};
    }
    for (int layer = static_cast<int>(activeLayer) - 1; layer >= 0; --layer)
    {
        const std::uint8_t kind = kinds[static_cast<std::size_t>(layer)];
        if (kind != kEmpty && kind != kInterest)
        {
            return {static_cast<std::uint8_t>(layer), kind};
        }
    }
    // Nothing beneath it: read as no state yet. A held member is not handed for it; a waiting further player does not
    // take the member until the interest clears (its AI is walking it).
    return {};
}

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
    if (SignalsArrival(kind) && layer == kLayerLevel)
    {
        return Hold::Arrival;
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
