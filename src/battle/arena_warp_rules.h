#pragma once

// The double click that brings a further player's character into the fight (battle/arena_warp.h applies it on the
// Dash press): two presses within kDoubleClickMs are one double click, and a pair once spent starts afresh.
// Host-tested (tests/battle_rules_test.cpp).

#include <cstdint>

namespace lss::battle::arena_warp_rules {

constexpr std::uint64_t kDoubleClickMs = 400;

struct DoubleClick
{
    std::uint64_t lastPress = 0;  // the first press of a pair (0: none waiting)

    /// A press at `now` (milliseconds): true on the second press within kDoubleClickMs of the first; the pair is then
    /// spent. A press later than that starts a new pair.
    constexpr bool Press(std::uint64_t now) noexcept
    {
        if (lastPress != 0 && now - lastPress <= kDoubleClickMs)
        {
            lastPress = 0;
            return true;
        }
        lastPress = now;
        return false;
    }
};

}  // namespace lss::battle::arena_warp_rules
