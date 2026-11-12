#pragma once

// The battle switch's rules in co-op (battle/party_switch.h applies the first, players/session.h the others), as pure
// functions over what the game reports; tested on the host (tests/party_switch_rules_test.cpp).

#include <cstdint>

namespace lss::battle::party_switch_rules {

/// Whether the d-pad's cycle runs as the game's forced switch runs it (its two-standing gate skipped). The game's gate
/// wants two members standing besides a downed leader, and in co-op the other players' members do not count, so a downed
/// player could never switch by hand; the game's own switch for a downed leader skips the gate, and the mod refuses that
/// one, so the player's own press runs it that way instead.
constexpr bool ForcedCycle(bool coop, bool leaderDown) noexcept
{
    return coop && leaderDown;
}

/// A player's own switch holds while the fight lasts: the row of the party order is not followed meanwhile (a party apply
/// or the slow check would send the player back), and the switch ends with the fight, when the player takes the row's
/// member again as the game sends player 1 back to the field leader.
constexpr bool OwnSwitchHolds(bool ownSwitch, bool inBattle) noexcept
{
    return ownSwitch && inBattle;
}

/// The fight's end is read through this many ticks in a row of the game's in-battle test saying no: a blip of the test
/// never ends a switch.
inline constexpr std::uint32_t kOwnSwitchEndTicks = 10;

constexpr bool OwnSwitchEnds(bool ownSwitch, std::uint32_t outOfBattleTicks) noexcept
{
    return ownSwitch && outOfBattleTicks >= kOwnSwitchEndTicks;
}

}  // namespace lss::battle::party_switch_rules
