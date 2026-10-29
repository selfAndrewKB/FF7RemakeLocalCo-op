#pragma once

// Whether a live AutoWalk act leashes player 1 to a further player's member (players/lead_gate; research/
// RE-lead-unlock.md §3, §5). Pure: no engine access, host-tested (tests/leash_rules_test.cpp).
//
// The level writes a leash on the default party leader as an AutoWalk act (Type 56) among a state's enter actions: while
// the slot of object Str[0] holds state Str[1], the act measures the leader's distance to Str[2] (a character's tag or a
// point), caps the leader's walk by distance bands, looks at Str[4] past the last band (Int[6]) and turns the leader
// back past Flt[0] (Int[4]). Aimed at a member a player holds, the leash keeps player 1 from going on until that member
// walks ahead, which in single player its own AI does (Ch. 4: Tifa after the 1st Feeler battle; Ch. 8: Aerith's walk
// home). Such a lead is a guide even when nothing waits on its arrival.

#include <cstddef>
#include <string_view>

namespace lss::players::leash_rules {

/// An AutoWalk act's arguments the rule reads (empty text for an absent argument, 0 for an absent number).
struct AutoWalkAct
{
    std::wstring_view object;         // Str[0]: the manager object whose state keeps the act running
    std::wstring_view state;          // Str[1]: that state
    std::wstring_view target;         // Str[2]: what the leader's distance is measured to
    std::wstring_view lookAt;         // Str[4]: what the camera looks at past the last band
    int               turnBack = 0;   // Int[4]: the turn-back is on
    int               lookAtOn = 0;   // Int[6]: the look-at band fires
};

/// Case-insensitive equality of two names (the game's FName compare); two empty names are not a match.
constexpr bool SameName(std::wstring_view a, std::wstring_view b) noexcept
{
    if (a.empty() || a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        wchar_t x = a[i], y = b[i];
        x = x >= L'A' && x <= L'Z' ? static_cast<wchar_t>(x - L'A' + L'a') : x;
        y = y >= L'A' && y <= L'Z' ? static_cast<wchar_t>(y - L'A' + L'a') : y;
        if (x != y)
        {
            return false;
        }
    }
    return true;
}

/// Whether `act` would leash the leader to the member whose tag is `memberTag` (AActor::Tags[0]) and whose level object
/// is `memberObject`, were it running: it measures to the member or looks at it, and it turns the leader back or looks
/// at the member when far. The first filter of the gate's search over the level's AutoWalk acts.
constexpr bool AimedAndArmed(const AutoWalkAct& act, std::wstring_view memberTag, std::wstring_view memberObject) noexcept
{
    const bool aimed = SameName(act.target, memberTag) || SameName(act.lookAt, memberObject);
    const bool armed = act.turnBack != 0 || act.lookAtOn != 0;
    return aimed && armed;
}

/// Whether `act`, an enter action of the slot `slotObject` / `slotState`, leashes the leader to that member: it runs
/// (that slot holds its state) and it is aimed and armed.
constexpr bool LeashesMember(const AutoWalkAct& act, std::wstring_view slotObject, std::wstring_view slotState,
                             std::wstring_view memberTag, std::wstring_view memberObject) noexcept
{
    const bool runs = SameName(act.object, slotObject) && SameName(act.state, slotState);
    return runs && AimedAndArmed(act, memberTag, memberObject);
}

}  // namespace lss::players::leash_rules
