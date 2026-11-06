#pragma once

// CharaPop's Light/Middle rule, as its evaluators apply it to one camera (research/RE-npc-near-player-two.md,
// RE-world-two-views.md: CharaPopEntry_EvalLOD_Light_PrimaryCam and _Middle_PrimaryCam), for
// the entries a further player's camera needs (world/npc_detail.h). Pure, host-tested (tests/charapop_rule_test.cpp).
//
//   paused  when distance > should-tick and (in the FOV cone ? distance > in-camera : distance > out-of-camera)
//   hidden  when distance > draw distance (the entries that hide only)
//
// The cone is a half-angle (EndPCM_IsLocationInFOV: the angle off the camera's forward <= the FOV value). The
// in-camera distance gains the enemy bonus for enemy entries (category 3). Remake's evaluator also adds an audience
// bonus and spares some hiding entries by a class it computes from the state manager each time; the rule leaves the
// class out, which can only raise an entry the game would have kept anyway, never miss one. `scale` widens every
// distance and `extraDegrees` the cone (1 and 0 for the game's own rule; more to keep an entry already raised, less
// to ask whether player 1's camera keeps it with room to spare).

namespace lss::world::charapop {
struct LodDistances
{
    float fovDegrees  = 0.0f;
    float shouldTick  = 0.0f;
    float inCamera    = 0.0f;
    float outCamera   = 0.0f;
    float enableDraw  = 0.0f;
    float enemyAdded  = 0.0f;
};

struct EntryView
{
    float distance     = 0.0f;  // cm, from the camera
    float angleDegrees = 0.0f;  // off the camera's forward
    bool  hides        = false;  // has the draw-distance hide
    bool  enemy        = false;
};

/// What the game would give the entry, judged from one camera.
struct Keeps
{
    bool awake = false;  // not tick-paused
    bool shown = false;  // not distance-hidden (only entries that hide)
};

constexpr Keeps KeepsOf(const LodDistances& lod, const EntryView& view, float scale = 1.0f,
                        float extraDegrees = 0.0f) noexcept
{
    const bool  inCone   = view.angleDegrees <= lod.fovDegrees + extraDegrees;
    const float inCamera = lod.inCamera + (view.enemy ? lod.enemyAdded : 0.0f);
    Keeps       keeps;
    keeps.awake = view.distance <= lod.shouldTick * scale || view.distance <= (inCone ? inCamera : lod.outCamera) * scale;
    keeps.shown = view.hides && view.distance <= lod.enableDraw * scale;
    return keeps;
}

/// True when, judged from this camera, the game would keep the entry ticking (or shown).
constexpr bool KeepsAwakeOrShown(const LodDistances& lod, const EntryView& view, float scale = 1.0f,
                                 float extraDegrees = 0.0f) noexcept
{
    const Keeps keeps = KeepsOf(lod, view, scale, extraDegrees);
    return keeps.awake || keeps.shown;
}

/// True when player 1's camera already gives the entry everything a further player's camera asks for, so the game
/// alone keeps it as that player needs and raising it would add only cost.
constexpr bool Covers(const Keeps& one, const Keeps& two) noexcept
{
    return (!two.awake || one.awake) && (!two.shown || one.shown);
}

// The margins that keep an entry from toggling on the edge: a raised entry stays raised until it is this far
// past a further player's distances, or this far inside player 1's.
inline constexpr float kKeepScale   = 1.1f;
inline constexpr float kKeepDegrees = 5.0f;

/// Whether to raise an entry not raised yet: a further player's view, at `reach` times the game's distances,
/// keeps it, and player 1's camera (when read) does not already.
constexpr bool ShouldRaise(const LodDistances& lod, const EntryView& two, const EntryView* one, float reach) noexcept
{
    const Keeps forTwo = KeepsOf(lod, two, reach);
    if (!forTwo.awake && !forTwo.shown)
    {
        return false;
    }
    return one == nullptr || !Covers(KeepsOf(lod, *one), forTwo);
}

enum class Hold
{
    Keep,             // still needed
    OutOfReach,       // past the further players' distances (with the margin)
    PlayerOneCovers,  // player 1's camera keeps it (with the margin)
};

/// For an entry the mod raised: whether it is still needed, and why not.
constexpr Hold HoldOf(const LodDistances& lod, const EntryView& two, const EntryView* one, float reach) noexcept
{
    const Keeps forTwo = KeepsOf(lod, two, reach * kKeepScale, kKeepDegrees);
    if (!forTwo.awake && !forTwo.shown)
    {
        return Hold::OutOfReach;
    }
    if (one != nullptr && Covers(KeepsOf(lod, *one, 1.0f / kKeepScale, -kKeepDegrees), forTwo))
    {
        return Hold::PlayerOneCovers;
    }
    return Hold::Keep;
}

// The same two questions for several further players' views at once (the detail reach is the
// union of every further player's): an entry is raised when any of their views needs it and player 1's camera does
// not cover what that view asks; a raised entry is kept while any of their views still needs it.

/// Whether to raise an entry not raised yet: some further player's view, at `reach` times the game's distances,
/// keeps it, and player 1's camera (when read) does not already.
constexpr bool ShouldRaise(const LodDistances& lod, const EntryView* views, int count, const EntryView* one,
                           float reach) noexcept
{
    for (int i = 0; i < count; ++i)
    {
        if (ShouldRaise(lod, views[i], one, reach))
        {
            return true;
        }
    }
    return false;
}

/// For an entry the mod raised: kept while any further player's view still needs it; else player 1's camera covers
/// it if it covers every view that still reaches it, and out of reach when none reaches it.
constexpr Hold HoldOf(const LodDistances& lod, const EntryView* views, int count, const EntryView* one, float reach) noexcept
{
    bool covered = false;
    for (int i = 0; i < count; ++i)
    {
        const Hold hold = HoldOf(lod, views[i], one, reach);
        if (hold == Hold::Keep)
        {
            return Hold::Keep;
        }
        covered = covered || hold == Hold::PlayerOneCovers;
    }
    return covered ? Hold::PlayerOneCovers : Hold::OutOfReach;
}

}  // namespace lss::world::charapop
