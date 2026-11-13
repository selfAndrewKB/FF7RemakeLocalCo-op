#pragma once

// Whether a further player keeps a member the level walks to its marks, and when the game takes it over at a mark
// (the player keeps control without breaking the scripting, guided by the mod's own markers;
// research/RE-holder-keeps-lead.md, RE-lead-marker.md).
//
// The lead gate (players/lead_gate.h) finds a lead or a run to a mark the level needs the member to finish (a waiter
// on its arrival, the story's marker on it, a further move in its own chain, a leash on player 1). Left to the gate
// alone, every such move goes to the member's AI (a guide). With the holder keeping it:
//   - the player keeps the member and is shown the move's point (hud/lead_mark: the main-objective diamond in that
//     player's view only, a pin on that player's own minimap);
//   - when the move signals its arrival to the level, within kHandRadius of the point (2D, as the game measures an
//     arrival) the member goes to its AI: the game's own Enter of the move runs again from there (within the
//     row's GoalRadius - 15 cm, 35 to 85 cm on the rows read, it arrives where it stands, else it walks
//     the last steps; a Lead then waits for player 1 to come within about 8-10 m, as in single player) and signals;
//   - a spot on the way that only a companion's AI may take now (its level state's NPC-only: a ladder the level opens
//     to everyone only after the companions climbed it) counts as reached within kCrossingRadius of it: no player can
//     take it, so the AI takes it and finishes the move, as the game has it (research/RE-ladder-spot-offer.md);
//   - it stays on its AI until the level writes its next setting (the move's state changes), then comes back;
//   - a move with no arrival trigger (Tifa's Ch. 4 lead: only the leash needs her) signals nothing: kept throughout.
// A move whose point is no field point of the loaded level (a follow, an actor's name) cannot be marked: a guide, as
// before, and so is every needed move under `lead-holder guide`.
//
// Pure (host-tested in tests/lead_keep_rules_test.cpp).

#include "players/field_hold.h"

#include <cstdint>
#include <string_view>

namespace lss::players::lead_keep_rules {

enum class Verdict : std::uint8_t
{
    Staging,  // nothing needs the move finished: the player's, no marker
    Guide,    // to its AI until the setting ends (no point to mark, or `lead-holder guide`)
    Keep,     // the player's, the point marked
    AtMark,   // brought to the point: to its AI until the setting ends
};

/// How near the point (cm, 2D) the member goes to its AI. Wider than every stepless arrival read (85 cm), so the AI
/// always has the last steps to walk or none; narrow enough that the player walks nearly all of it. The hold check runs
/// every 10 ticks: a sprint (about 6 m/s) moves 1 m between checks, inside the 4 m circle.
inline constexpr float kHandRadius = 200.0f;

/// How near an AI-only spot (cm, 2D, from the spot actor's place) a kept move's member goes to its AI. Chosen on the
/// expectation that a ladder's actor stands near the ladder's line, so a member at its foot or top is within it; the
/// lead gate logs the nearest such spot's distance at each kept move's start, which measures that. 2D only: a member
/// at the top of a tall ladder stands far above its actor.
inline constexpr float kCrossingRadius = 250.0f;

/// The member stands within kHandRadius of the point, `dx`, `dy` apart (cm).
constexpr bool Reached(float dx, float dy) noexcept
{
    return dx * dx + dy * dy <= kHandRadius * kHandRadius;
}

/// The verdict for a move. `needed`: the gate's reasons (a waiter, the marker, a further move, a leash). `keeps`: the
/// `lead-holder keeps` switch. `pointKnown`: the move's destination is a field point of the loaded level. `signals`: the
/// move names an arrival trigger. `reached`: the member is (or was, this setting) within kHandRadius of the point.
/// `intoOwnBox`: the step moves on at the member's own armed story box, which the run carries it into on the level's clock
/// (players/lead_gate OwnBoxAhead): a guide whatever the switch and whether or not anything else needs it, since a player
/// walking it there may come too late.
constexpr Verdict Decide(bool needed, bool keeps, bool pointKnown, bool signals, bool reached, bool intoOwnBox = false) noexcept
{
    if (intoOwnBox)
    {
        return Verdict::Guide;
    }
    if (!needed)
    {
        return Verdict::Staging;
    }
    if (!keeps || !pointKnown)
    {
        return Verdict::Guide;
    }
    return signals && reached ? Verdict::AtMark : Verdict::Keep;
}

/// The level moves a scene opens on, by their state names: the member runs ahead and stands at the scene's spot, and the
/// scene starts when the leader reaches it and talks to the member. The run and the stand are one piece of staging the
/// member's AI must play whole: a player walking the run brings the leader's talk before the level has armed the scene,
/// and the talk plays as a plain line (Cloud's run from Aerith to the Corneo mansion front after the dance). Excluded
/// from the lead rule by name, whatever the switch: the member goes to its AI at the first of these states and comes
/// back when its state leaves them (the scene itself takes it as every scene does). Lookups compare without case, as the
/// game compares names.
inline constexpr std::wstring_view kSceneLeadIns[] = {
    L"scg080_SLU5B_40_Cloud_RunfromAerith",
    L"scg080_SLU5B_40_Cloud_RunStayfromAerith",
};

constexpr bool SceneLeadIn(std::wstring_view state) noexcept
{
    for (const std::wstring_view name : kSceneLeadIns)
    {
        if (field_hold::SameIgnoringCase(name, state))
        {
            return true;
        }
    }
    return false;
}

/// The verdicts that hand the member to its AI; each holds until the move's setting ends.
constexpr bool ToAi(Verdict verdict) noexcept
{
    return verdict == Verdict::Guide || verdict == Verdict::AtMark;
}

constexpr const wchar_t* Name(Verdict verdict) noexcept
{
    switch (verdict)
    {
    case Verdict::Staging:
        return L"staging (the player's)";
    case Verdict::Guide:
        return L"GUIDE (to its AI, the player's view on it)";
    case Verdict::Keep:
        return L"KEEP (the player's; its point marked in that player's view)";
    case Verdict::AtMark:
        return L"AT THE MARK (to its AI: it finishes the arrival, then back to the player at the level's next setting)";
    }
    return L"?";
}

}  // namespace lss::players::lead_keep_rules
