#pragma once

// The rules of world/story_boxes, without the game (host-tested in tests/story_box_rules_test.cpp): which of a held
// member's story-box entries wait, when a waiting one fires, and whether player 1's character has come to the box.
// research/RE-held-member-story-boxes.md.

#include <cmath>
#include <cstdint>

namespace lss::world::story_box_rules {

/// What the hook knows about a story box a further player's member walked into.
struct Entry
{
    bool heldByPlayer;      // the member's PLAYER slot holds a controller (its player walks it, not its AI)
    bool levelSentIt;       // the level has the member on a move of its own, whoever walks it (players/lead_gate OnLevelMove)
    bool groupSet;          // the box's ObjectLayout row names an actor group (a group box)
    bool groupNamesLeader;  // that group lists the party leader's tag (a party box: Cloud fires it on arrival)
};

/// The entry waits (the game's handler is not called now): a member-only story box entered by a member its player walks
/// ahead of the story. A party box, a group-less box, a member on its AI or on a move the level sent are the game's: while
/// the level has the member on any move (a staging run its player walks included), the level is at a moving step and the
/// member's boxes are due, as on the companion's run in single player.
constexpr bool HoldBack(const Entry& e) noexcept
{
    return e.heldByPlayer && !e.levelSentIt && e.groupSet && !e.groupNamesLeader;
}

/// What a waiting entry's tick sees.
struct Waiting
{
    bool boxGone;          // the box is no longer registered under its tag (its level unloaded, a load)
    bool seated;           // the member is still its player's seat's member
    bool handedToAi;       // its PLAYER slot is empty: the game or the session handed it to its AI
    bool inCinema;         // its actor mode is CINEMA: the game dispatches no box then either
    bool levelMoves;       // the level has it on a move of its own, whoever walks it (players/lead_gate OnLevelMove)
    bool playerOneReached; // player 1's character has come to the box (Reaches below)
    bool awaitsRun;        // the member was brought back for the level's run, which passes through this box (world/run_catch_up)
    bool ranIn;            // since then the game's own enter ran for the member and this box (seen at the redirect)
    bool runOver;          // the run is over without it: the member at the run's point, or kAwaitRunLimitMs gone (RunOver)
};

enum class Step : std::uint8_t
{
    Wait,    // keep it
    Replay,  // the game's begin handler now (and its end handler when the member is outside)
    Lost,    // the box is gone first: dropped, with an Info line
    Taken,   // the member on its AI walked into the box: the game's own enter ran (no replay)
};

/// The member enters the box "now", the moment a companion would in single player: when the level moves it or hands it
/// to its AI, when its player leaves, or when player 1's character comes to the box. Never during a cutscene (the game's
/// own sweep dispatches nothing in the CINEMA mode).
constexpr Step Next(const Waiting& w) noexcept
{
    if (w.boxGone)
    {
        return Step::Lost;
    }
    if (w.inCinema)
    {
        return Step::Wait;
    }
    // Brought back for the level's run through the box: the run enters it, as a companion's does in single player. Its
    // AI's hand is not the moment; the member's own body is. Late, never lost: its player leaving, player 1 coming to the
    // box, or the member back with its player (the run's setting over) fire it as before.
    if (w.awaitsRun)
    {
        if (w.ranIn)
        {
            return Step::Taken;  // the game's own enter ran (seen at the redirect): never a second one
        }
        return !w.seated || w.playerOneReached || !w.handedToAi || w.runOver ? Step::Replay : Step::Wait;
    }
    if (!w.seated || w.handedToAi || w.levelMoves || w.playerOneReached)
    {
        return Step::Replay;
    }
    return Step::Wait;
}

/// A box as the engine places it: the collision component's ComponentToWorld (rotation quaternion x y z w,
/// translation, scale) and the box component's unscaled half-size, all in centimetres.
struct Box
{
    float rotation[4];
    float at[3];
    float scale[3];
    float extent[3];
};

/// How far outside the box player 1's character may be and still count as having come to it: a following companion
/// trails its leader by a few metres, so it enters about when the leader passes within this of the box.
inline constexpr float kReachMarginCm       = 300.0f;
inline constexpr float kReachMarginHeightCm = 200.0f;

/// `point` (world cm) in the box's own frame: the inverse rotation of its offset from the box's centre.
inline void ToLocal(const Box& box, const float point[3], float local[3]) noexcept
{
    const float d[3] = {point[0] - box.at[0], point[1] - box.at[1], point[2] - box.at[2]};
    // The inverse rotation: the conjugate's vector part (-x, -y, -z) with the same w.
    const float ux = -box.rotation[0], uy = -box.rotation[1], uz = -box.rotation[2], w = box.rotation[3];
    // v' = v + 2w (u x v) + 2 u x (u x v)
    const float cx = uy * d[2] - uz * d[1];
    const float cy = uz * d[0] - ux * d[2];
    const float cz = ux * d[1] - uy * d[0];
    local[0] = d[0] + 2.0f * (w * cx + (uy * cz - uz * cy));
    local[1] = d[1] + 2.0f * (w * cy + (uz * cx - ux * cz));
    local[2] = d[2] + 2.0f * (w * cz + (ux * cy - uy * cx));
}

/// `point` (world cm) lies within the box grown by the margins (horizontally `marginCm`, vertically `marginHeightCm`).
inline bool Reaches(const Box& box, const float point[3], float marginCm = kReachMarginCm,
                    float marginHeightCm = kReachMarginHeightCm) noexcept
{
    float local[3];
    ToLocal(box, point, local);
    const float margins[3] = {marginCm, marginCm, marginHeightCm};
    for (int i = 0; i < 3; ++i)
    {
        const float half = std::fabs(box.extent[i] * box.scale[i]) + margins[i];
        if (!(std::fabs(local[i]) <= half))  // a NaN never reaches
        {
            return false;
        }
    }
    return true;
}

/// A run the member was brought back for is over without taking its box when the member stands within kRunArrivedCm of
/// the run's point (2D: it has arrived, the box is behind it or its path went round it), when it has not moved kRunStillCm
/// for kRunStillMs (a run stopped short: blocked by a character), or after kAwaitRunLimitMs. Then the held enter fires:
/// late, never lost (a run that misses its box would otherwise leave the step waiting for good).
/// No level's clock is assumed: the Ch. 5 dash from player 1's side takes about 8 s and the talk's cue comes
/// about 12.5 s after the hand, and a run that stops short is over 2 s after it stops.
inline constexpr float        kRunArrivedCm    = 200.0f;
inline constexpr float        kRunStillCm      = 50.0f;
inline constexpr std::int64_t kRunStillMs      = 2000;
inline constexpr std::int64_t kAwaitRunLimitMs = 20000;

/// `dx`, `dy`: the member's offset from the run's point (cm); `elapsedMs` since it was brought back; `stillMs` since it
/// last moved kRunStillCm.
constexpr bool RunOver(float dx, float dy, std::int64_t elapsedMs, std::int64_t stillMs) noexcept
{
    return dx * dx + dy * dy <= kRunArrivedCm * kRunArrivedCm || stillMs >= kRunStillMs || elapsedMs >= kAwaitRunLimitMs;
}

/// How much wider than the box a straight run may pass and still count as running through it: a character's capsule
/// (about 35 cm) enters the box before its centre does. Vertically as Reaches (stairs, slopes).
inline constexpr float kRunMarginCm       = 50.0f;
inline constexpr float kRunMarginHeightCm = kReachMarginHeightCm;

/// The straight run from `from` to `to` (world cm) passes through the box grown by the margins: the segment against
/// the box's slabs in its own frame. False for a NaN or an infinity anywhere.
inline bool RunCrosses(const Box& box, const float from[3], const float to[3], float marginCm = kRunMarginCm,
                       float marginHeightCm = kRunMarginHeightCm) noexcept
{
    float a[3], b[3];
    ToLocal(box, from, a);
    ToLocal(box, to, b);
    const float margins[3] = {marginCm, marginCm, marginHeightCm};
    float enter = 0.0f, leave = 1.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float half = std::fabs(box.extent[i] * box.scale[i]) + margins[i];
        const float d    = b[i] - a[i];
        if (!(half >= 0.0f) || !std::isfinite(a[i]) || !std::isfinite(b[i]))
        {
            return false;  // a NaN or an infinity
        }
        if (std::fabs(d) < 1e-6f)
        {
            if (std::fabs(a[i]) > half)
            {
                return false;  // parallel to this slab and outside it
            }
            continue;
        }
        float t0 = (-half - a[i]) / d, t1 = (half - a[i]) / d;
        if (t0 > t1)
        {
            const float t = t0;
            t0 = t1;
            t1 = t;
        }
        enter = t0 > enter ? t0 : enter;
        leave = t1 < leave ? t1 : leave;
        if (enter > leave)
        {
            return false;
        }
    }
    return true;
}

}  // namespace lss::world::story_box_rules
