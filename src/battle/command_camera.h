#pragma once

// Each seat's Tactical Mode target camera: the field camera's Command flavor (13) reads the seat's own menu.
//
// While a player's command controller sits in a target list (its states 8 and 9), the controller's tick turns the
// field camera's Command flavor on, and the flavor, inside the camera operator's own tick, frames the target the
// list cursor is on. It finds that target every frame through two of the game's single slots: the UI system's
// command controller (EndUISystem[19]) and the input object's focus path (the row that has focus). A seat's
// operator ticks in its primary, operator, leader and target-field scopes, so the flavor runs on the seat's own
// operator; but without the UI slot and the focus scope it read player 1's controller and player 1's focus: no
// open menu of its own there, nothing to frame (the user's 2026-10-22 report; research/RE-command-camera-two.md).
// This module:
//   - holds the seat's UI slot and focus scope around its operator tick while its menu is in battle (Rebirth's
//     rule: player 2's camera update in its UI context in battle), so the flavor reads the seat's own list;
//   - reads the flavor's per-frame aim (its vtable slot replaced, the game's function called): per seat, how often
//     it ran on that seat's operator, how often with the seat's own controller in the UI slot, the target it
//     settled on and its on/started bytes (`command-camera.aims`; a Debug line per new target).
// Game thread only. State owned here: the counters per seat; the scope lives on the operator tick's stack.

#include <optional>

#include "players/scopes.h"

namespace lss::platform {
class GameImage;
}

namespace lss::battle::command_camera {

/// Checks the flavor's vtable slot and replaces it with the reading. False (logged) if it differs; the scope below
/// works either way.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The seat's UI slot and focus, held around its operator tick while its command controller is set up and in
/// battle. A refusal (another focus scope open, or the seat's not bound) is counted and logged.
class TickScope
{
public:
    explicit TickScope(int seat) noexcept;
    ~TickScope() = default;
    TickScope(const TickScope&)            = delete;
    TickScope& operator=(const TickScope&) = delete;

    bool Held() const noexcept { return scope_.has_value(); }

private:
    std::optional<players::scopes::Scope> scope_;
};

}  // namespace lss::battle::command_camera
