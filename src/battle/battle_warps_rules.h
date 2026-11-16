#pragma once

// Who the game warps at a battle's start and end, as the setting decides it without the game (battle/battle_warps.h
// applies it at the game's three calls). Host-tested (tests/battle_rules_test.cpp).

#include <cstdint>

namespace lss::battle::battle_warps_rules {

/// The setting `[Battle] battle-warps` (platform/settings.h kBattleWarps, the words `on` and `off` in this order).
enum class Mode : std::uint8_t
{
    Game,         // on: the game's warps for everyone
    PlayersStay,  // off: every player's own character stays; the others are warped
};

/// Whether the setting keeps this member where it is: seat 0 is player 1's character, which the game never warps
/// (it warps toward the leader), so only the seats' characters are the question.
constexpr bool Stays(Mode mode, int seat) noexcept
{
    return mode == Mode::PlayersStay && seat != 0;
}

}  // namespace lss::battle::battle_warps_rules
