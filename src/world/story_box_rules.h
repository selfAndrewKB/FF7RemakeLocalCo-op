#pragma once

// The rules of world/story_boxes, without the game (host-tested in tests/story_box_rules_test.cpp): which of a held
// member's story-box entries wait, when a waiting one fires, and whether player 1's character has come to the box.
// research/RE-held-member-story-boxes.md §3.4, §4, §7; the user's conditions of 2026-10-30 (session History 12:17, 12:48).

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
};

enum class Step : std::uint8_t
{
    Wait,    // keep it
    Replay,  // the game's begin handler now (and its end handler when the member is outside)
    Lost,    // the box is gone first: dropped, with an Info line
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

/// `point` (world cm) lies within the box grown by the margins (horizontally `marginCm`, vertically `marginHeightCm`).
inline bool Reaches(const Box& box, const float point[3], float marginCm = kReachMarginCm,
                    float marginHeightCm = kReachMarginHeightCm) noexcept
{
    const float d[3] = {point[0] - box.at[0], point[1] - box.at[1], point[2] - box.at[2]};
    // The inverse rotation: the conjugate's vector part (-x, -y, -z) with the same w.
    const float ux = -box.rotation[0], uy = -box.rotation[1], uz = -box.rotation[2], w = box.rotation[3];
    // v' = v + 2w (u x v) + 2 u x (u x v)
    const float cx = uy * d[2] - uz * d[1];
    const float cy = uz * d[0] - ux * d[2];
    const float cz = ux * d[1] - uy * d[0];
    const float local[3] = {d[0] + 2.0f * (w * cx + (uy * cz - uz * cy)),
                            d[1] + 2.0f * (w * cy + (uz * cx - ux * cz)),
                            d[2] + 2.0f * (w * cz + (ux * cy - uy * cx))};
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

}  // namespace lss::world::story_box_rules
