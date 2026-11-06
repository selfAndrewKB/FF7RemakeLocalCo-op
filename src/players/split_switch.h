#pragma once

// The game viewport's stock split switch, UGameViewportClient::bDisableSplitScreenOverride
// (research/RE-layout-fov.md [CONFIRMED static]): while it is set, UpdateActiveSplitscreenType picks the None
// layout whatever the player count, player 1's view fills the screen and every further local player gets a zero-size
// view (CalcSceneViewInitOptions refuses it: no scene view, no render, no listener update, no HUD pass). 4.18 has no
// reflected setter (SetForceDisableSplitscreen is 4.26's), so the byte is read and written directly, guarded, on the
// client the engine names (GEngine's GameViewport); LayoutPlayers runs on every redraw, so a write during the engine
// tick shows in that frame's draw. The policy (set only by the mod, cleared only when the mod set it, failures said
// once) is players/single_view.h's. Game thread.

#include <optional>

namespace lss::platform {
class GameImage;
}

namespace lss::players::split_switch {

/// Rebases GEngine. False (logged) if it is outside the image: the switch then reads as unknown and is never written.
bool Install(const platform::GameImage& image) noexcept;

/// The switch; empty while GEngine or its game viewport client is missing, or the read faults.
std::optional<bool> Read() noexcept;

/// Writes the switch; false (writing nothing) under the same conditions.
bool Write(bool disabled) noexcept;

}  // namespace lss::players::split_switch
