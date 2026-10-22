#pragma once

// The command menus' shared engine state, as rules without the game (battle/command_menu_tick.h applies them;
// Rebirth's player_two/menu_rules held the two-player form).

#include <algorithm>
#include <cstdint>
#include <optional>

namespace lss::battle::command_menu_rules {

/// The world's battle-command speed slot (the menu's time slow): every command controller's tick writes it, the
/// game's first and then each seat's in order, and the last write wins. So that any menu slows the world, player
/// 1's write starts the frame's least value and each seat's write takes the least of the frame's so far and its
/// own. A seat's write in a frame where player 1's tick wrote nothing goes as asked and is counted: the game's tick
/// writes every frame (RE-battle.md §2.7), so the count stays 0 unless that changes.
class SlowWrites
{
public:
    /// A new engine frame: no menu has written yet.
    void BeginFrame() noexcept { least_.reset(); }

    /// The value seat `seat`'s tick writes (0: player 1's, the game's) for the `value` its menu asked.
    float Write(int seat, float value) noexcept
    {
        if (seat != 0 && !least_)
        {
            ++alone_;
        }
        least_ = seat == 0 || !least_ ? value : std::min(*least_, value);
        return *least_;
    }

    /// A seat's writes made with no write of player 1's in the frame (written as asked).
    std::uint32_t Alone() const noexcept { return alone_; }

private:
    std::optional<float> least_;  // the frame's least value so far, once a menu wrote
    std::uint32_t        alone_ = 0;
};

}  // namespace lss::battle::command_menu_rules
