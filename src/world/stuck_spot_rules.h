#pragma once

// Which level states a field-action spot's pass has left on, judged from the level's data alone
// (research/RE-stuck-spot-repair.md). A pass through a spot sends the spot's start triggers and, when it ends, its end
// triggers; in single player always both. So after any complete pass no level object rests in a state that a pass's start
// entered and the same pass's end leaves: an object resting there, while nobody is in a field action, was put there by a
// start whose end never came (a further player's pass that sent player 1's start signals and not his end). Such a state
// is "held" by the pass, and the pass's end triggers are what player 1's own pass would send to end it.
// One-way changes (a start moves something its end does not move back), box-driven area toggles (their triggers are no
// spot's) and a camera meant to stay on between two different spots (one spot's start, another's end) are not held.
// Pure: host tests in tests/stuck_spot_rules_test.cpp.

#include <array>
#include <cstdint>
#include <cwctype>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lss::world::stuck_spot_rules {

using Name = std::int64_t;  // an engine name's 8 bytes; 0 is None

/// A spot's six signals, as the spot actor holds them.
struct Signals
{
    Name defaultStart = 0, defaultEnd = 0, leftStart = 0, leftEnd = 0, rightStart = 0, rightEnd = 0;
};

/// One way through a spot: the starts its pass sends (the default and the side's), and its ends in the order the game's
/// exit sends them (the side's, then the default). 0 where the spot names none.
struct Pass
{
    std::array<Name, 2> starts{};
    std::array<Name, 2> ends{};
    bool                left = false;
};

inline bool Lists(const std::array<Name, 2>& names, Name name) noexcept
{
    return name != 0 && (names[0] == name || names[1] == name);
}

inline bool None(const std::array<Name, 2>& names) noexcept
{
    return names[0] == 0 && names[1] == 0;
}

inline bool SameNames(const std::array<Name, 2>& a, const std::array<Name, 2>& b) noexcept
{
    return (a[0] == b[0] && a[1] == b[1]) || (a[0] == b[1] && a[1] == b[0]);
}

/// The spot's distinct passes: the left one, then the right one when it sends other signals. A pass with no start or no
/// end cannot leave a state on and is left out.
inline std::vector<Pass> Passes(const Signals& s)
{
    std::vector<Pass> out;
    const auto add = [&](Name start, Name end, bool left) {
        const Pass pass{.starts = {s.defaultStart, start}, .ends = {end, s.defaultEnd}, .left = left};
        if (None(pass.starts) || None(pass.ends))
        {
            return;
        }
        for (const Pass& known : out)
        {
            if (SameNames(known.starts, pass.starts) && SameNames(known.ends, pass.ends))
            {
                return;
            }
        }
        out.push_back(pass);
    };
    add(s.leftStart, s.leftEnd, true);
    add(s.rightStart, s.rightEnd, false);
    return out;
}

/// State names compare as the engine compares names: without case.
inline bool SameText(std::wstring_view a, std::wstring_view b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::towlower(a[i]) != std::towlower(b[i]))
        {
            return false;
        }
    }
    return true;
}

/// One trigger a state waits on and the state it leads to ("" when the row names none).
struct Step
{
    Name         trigger = 0;
    std::wstring next;
};

/// Whether an object resting in state `current`, which one of `pass`'s ends would move to the state whose steps are
/// `back`, is held by the pass: from there one of the pass's starts leads back to `current` (a round trip through the
/// spot).
inline bool HeldBy(const Pass& pass, std::wstring_view current, std::span<const Step> back) noexcept
{
    for (const Step& step : back)
    {
        if (Lists(pass.starts, step.trigger) && SameText(step.next, current))
        {
            return true;
        }
    }
    return false;
}

/// What sending one of a pass's ends would do, as the dry run read it.
struct EndCheck
{
    Name         trigger = 0;
    int          moves   = 0;      // active states waiting on it with a next state: what its send moves
    int          held    = 0;      // of them, the ones the pass holds
    std::wstring rowRefusal;       // the row's own refusal (world/state_signal), empty when it may be sent
    std::uint8_t scope   = 0;      // 1 and 2: every active state waiting on it; 0 the sender's own; 3 listed objects
    std::wstring storyActs;        // an act the moves would finish or start that touches the story, empty when none
    bool         chained = false;  // a state it returns a held one to waits on one of the pass's ends again: the ends,
                                   // sent together, would move it on past where the dry run looked
};

/// Whether a state with steps `back` waits on one of `pass`'s ends.
inline bool WaitsOnEnds(const Pass& pass, std::span<const Step> back) noexcept
{
    for (const Step& step : back)
    {
        if (Lists(pass.ends, step.trigger))
        {
            return true;
        }
    }
    return false;
}

/// A pass's verdict: the ends to send (those that move something, in the exit's order), or why none is sent. `held` is
/// false when the pass holds nothing (nothing to report).
struct Plan
{
    bool              held = false;
    std::vector<Name> send;
    std::wstring      refusal;
};

/// Send the ends that move something when every one of them moves only states the pass holds, from a spot that may be
/// passed more than once, through a row that saves nothing, sets nothing directly, clears no history and reaches every
/// waiting state, with no story act finished or started on the way and no state it returns waiting on the ends again.
/// A waiting state whose next state could not be read counts as a move the pass does not hold.
inline Plan Judge(bool onceOnly, std::span<const EndCheck> ends)
{
    Plan plan;
    for (const EndCheck& end : ends)
    {
        plan.held = plan.held || end.held > 0;
    }
    if (!plan.held)
    {
        return plan;
    }
    if (onceOnly)
    {
        plan.refusal = L"a one-shot spot";
        return plan;
    }
    for (const EndCheck& end : ends)
    {
        if (end.moves == 0)
        {
            continue;
        }
        if (!end.rowRefusal.empty())
        {
            plan.refusal = end.rowRefusal;
        }
        else if (end.scope != 1 && end.scope != 2)
        {
            plan.refusal = L"its row's scope is not every waiting state (scope " + std::to_wstring(end.scope) + L")";
        }
        else if (end.held < end.moves)
        {
            plan.refusal = L"it would also move " + std::to_wstring(end.moves - end.held) + L" state(s) this pass did not leave on";
        }
        else if (!end.storyActs.empty())
        {
            plan.refusal = end.storyActs;
        }
        else if (end.chained)
        {
            plan.refusal = L"a state it returns waits on this pass's ends again";
        }
        if (!plan.refusal.empty())
        {
            plan.send.clear();
            return plan;
        }
        plan.send.push_back(end.trigger);
    }
    return plan;
}

}  // namespace lss::world::stuck_spot_rules
