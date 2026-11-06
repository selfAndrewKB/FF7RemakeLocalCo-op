#pragma once

// The enemy bar's class in a further column (research/RE-hud-battle-markers.md, RE-target-cursor-two.md), a pure rule
// tested on the host (tests/bar_class_test.cpp). hud/marker_replay keeps the engine reads, the calls and the log.
//
// The game's tick classifies each bar once, with player 1's displayed target: targeted = (a) the bar's source is
// that target, or (b) its main target, or (c) the "always show" case (the leader clamp skipped: the tick passes the
// skip byte to the targeted placement), or (d) the bar's own state (a flag, a status child, the recently-hit timer).
// It records only the kind it placed and the skip byte. A further column's class is the same rule with that
// column's keys: (a) and (b) from the column's own displayed target, (c) and (d) as the tick found them, where (d)
// is what remains of a targeted kind once (c) and player 1's keys are taken out.

namespace lss::hud::bar_class {

/// One player's displayed target and its main target (null: none).
struct Keys
{
    const void* own  = nullptr;
    const void* main = nullptr;
};

struct Verdict
{
    bool own      = false;  // the column's own displayed target
    bool main     = false;  // its main target (a locked part's owner)
    bool caseC    = false;  // the game's always-show case: the targeted placement without the leader clamp
    bool forced   = false;  // case (d): targeted by the bar's own state, whoever looks
    bool targeted = false;  // placed as a target in this column
};

/// `tickTargeted` and `tickSkip`: the kind and the skip byte the tick recorded for this bar (player 1's keys in
/// `one`); `two`: the further column's keys.
constexpr Verdict Classify(const void* source, Keys one, Keys two, bool tickTargeted, bool tickSkip) noexcept
{
    Verdict v;
    v.own   = source != nullptr && source == two.own;
    v.main  = source != nullptr && source == two.main;
    v.caseC = tickTargeted && tickSkip;
    const bool oneKeyed = source != nullptr && (source == one.own || source == one.main);
    v.forced   = tickTargeted && !v.caseC && !oneKeyed;
    v.targeted = v.own || v.main || v.caseC || v.forced;
    return v;
}

/// The mod's rule for player 1's own column, to check against the tick's choice (logged when they disagree): the
/// tick's kind must equal the rule with player 1's keys.
constexpr bool PlayerOneAgrees(const void* source, Keys one, bool tickTargeted, bool tickSkip) noexcept
{
    const Verdict v = Classify(source, one, one, tickTargeted, tickSkip);
    return v.targeted == tickTargeted;
}

}  // namespace lss::hud::bar_class
