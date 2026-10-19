#pragma once

// A reading on player 2's lock camera (RE-lock-camera-two.md §1.4).
//
// The field camera operator's Lock state follows the displayed target (player 2's, with its target copy in) but
// re-aims on the battle camera requests, which are keyed on the party leader. Player 2's operator ticks in the
// leader scope now; this module counts the request call's true returns per operator so a played session shows
// whose actions reach player 2's lock camera. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::lock_camera_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick: places the call redirect from the game thread (the call crosses a block).
void Tick() noexcept;

}  // namespace lss::battle::lock_camera_two
