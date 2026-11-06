#pragma once

// Which human an enemy counts as "the player" for one decision (battle/enemy_coin.h;
// research/RE-enemy-attention-coin.md). Pure, tested on the host.
//
// In the single-player game "the player" is the one character the player controls: the enemy scripts aim their
// moves at it and the hate rows weight it. With two or more humans each decision names one of them, so all get the
// player's weight equally:
//   - no further player in: the game's own answer, untouched;
//   - two or more humans' members eligible (the game's own rule: standing in the battle): a fair pick among them;
//   - one eligible: that one, so a knocked-out member never takes the weight from the other;
//   - none: player 1, the game's own answer.
// Seats are bits: bit 0 player 1 (the game's leader), bit 1 player 2, and so on.

#include <bit>
#include <cstdint>

namespace lss::battle::coin_choice {

inline constexpr int kGamesOwn = -1;  // no choice made: the game's own answer stands

enum class Why : std::uint8_t
{
    NoFurtherPlayer,
    Coin,             // two or more eligible
    OnlyEligible,     // just this one eligible
    NeitherEligible,  // player 1, as the game would
};

struct Choice
{
    int player = kGamesOwn;  // the seat index (0 = player 1), or kGamesOwn
    Why why    = Why::NoFurtherPlayer;
};

/// The n-th set bit of `mask` (n from 0), as a seat index.
constexpr int NthSet(unsigned mask, unsigned n) noexcept
{
    for (unsigned i = 0; i < n; ++i)
    {
        mask &= mask - 1;
    }
    return std::countr_zero(mask);
}

/// `seated`: the humans in (bit 0 always set once the game runs), `eligible`: whose member stands; `random` decides
/// the pick when two or more are eligible (any value; reduced modulo their number).
constexpr Choice Choose(unsigned seated, unsigned eligible, unsigned random) noexcept
{
    if ((seated & ~1u) == 0)
    {
        return {kGamesOwn, Why::NoFurtherPlayer};
    }
    const unsigned candidates = seated & eligible;
    const unsigned count      = static_cast<unsigned>(std::popcount(candidates));
    if (count == 0)
    {
        return {0, Why::NeitherEligible};
    }
    if (count == 1)
    {
        return {std::countr_zero(candidates), Why::OnlyEligible};
    }
    return {NthSet(candidates, random % count), Why::Coin};
}

}  // namespace lss::battle::coin_choice
