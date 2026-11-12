#pragma once

// Which push-button rows (a switch, a lever, a terminal; world/push_events.h) a player beyond player 1 may work, and how
// the row is run for that player's character, as pure functions over the row's names (tested on the host,
// tests/push_event_rules_test.cpp; the rows are in research/RE-further-player-levers.push-buttons.csv).
//
// A row names the characters it is authored for and, per character, four animation phases. The game builds its event's
// actors from those names and plays the named animations; a name it cannot resolve plays nothing and the event goes on
// (the game's own path for a character with nothing to play). So a row for one character whose every animation is a
// lever pull is run for the pressing player's character: as written when the row names that character, else with the
// character's own name in the row and no animation (the character stands at the lever while the gauge fills and the lever
// moves). Rows for two characters (a lift, a wall, a high touch) and rows whose animation is not a lever's (a container
// push, a loop) stay player 1's.

#include "players/field_hold.h"

#include <cstddef>
#include <string_view>

namespace lss::world::push_event_rules {

/// A lever animation of the shared set: `CMN_PC<type>_..._Lever...`.
constexpr bool LeverAnimation(std::wstring_view action) noexcept
{
    return action.starts_with(L"CMN_PC") && action.find(L"Lever") != std::wstring_view::npos;
}

struct Plan
{
    bool admitted    = false;  // the row is a player's to work
    bool asWritten   = false;  // the row names the player's character: nothing exchanged
    bool noAnimation = false;  // the character's name takes the row's, its animations left out
};

/// The plan for `memberTag` (the character's own name tag) at a row naming `actors` (count `actorCount`) with the
/// animations `actions` (count `actionCount`; empty names allowed).
constexpr Plan PlanFor(const std::wstring_view* actors, std::size_t actorCount, const std::wstring_view* actions, std::size_t actionCount,
                       std::wstring_view memberTag) noexcept
{
    if (actorCount != 1 || actors == nullptr)
    {
        return {};  // two characters at once, or none named: the story's, player 1's
    }
    for (std::size_t i = 0; i < actionCount; ++i)
    {
        if (!actions[i].empty() && !LeverAnimation(actions[i]))
        {
            return {};  // a container push, a loop animation: the character's own, not a lever
        }
    }
    const bool named = players::field_hold::SameIgnoringCase(actors[0], memberTag);  // the game compares its names without case
    return {.admitted = true, .asWritten = named, .noAnimation = !named};
}

/// An operation board (a quick press: wall and floor buttons, key consoles, elevator switches) by its target name. The
/// elevator switches stay player 1's: the cabin may carry the party with player 1 elsewhere, unseen.
constexpr bool OperationBoard(std::wstring_view target) noexcept
{
    return target.starts_with(L"OperationBoard_");
}

constexpr bool ElevatorBoard(std::wstring_view target) noexcept
{
    return OperationBoard(target) && target.find(L"ELVSwitch") != std::wstring_view::npos;
}

}  // namespace lss::world::push_event_rules
