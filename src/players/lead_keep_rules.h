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
//   - it stays on its AI until the level writes its next setting (the move's state changes), then comes back;
//   - a move with no arrival trigger (Tifa's Ch. 4 lead: only the leash needs her) signals nothing: kept throughout.
// A move whose point is no field point of the loaded level (a follow, an actor's name) cannot be marked: a guide, as
// before, and so is every needed move under `lead-holder guide`.
//
// Pure (host-tested in tests/lead_keep_rules_test.cpp).

#include <cstdint>

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
