#pragma once

// The camera in a view narrower than the game's image and spread across a window: two small corrections beside the narrow
// view's FOV (views/split_fov_rules.h; research/RE-narrow-view-camera.md).
//
// The sideways offset. The game's camera places itself from a pose whose aim offset is in world units (a town's row 48, an
// interior's 50, wall running and battle targeting 50), so the same offset fills about twice the screen share in a view half
// as wide, and the worst scripted rows push the character off it. Nothing in the camera reads the view's shape and no
// per-player lever for the offset exists, so this is a documented exception, as in Rebirth (camera/shoulder_scale): the one
// call that turns the aim offset into a world point (FRotator::RotateVector in AEndFieldCameraOperator's Tick) gets a copy with
// its sideways part multiplied by the view's factor (split_fov_rules::ShoulderFactor). The operator's own pose is not touched,
// so nothing accumulates and every later step follows the placed camera. Field and battle are one operator, one site.
// Rebirth's multiply sits on the pose before its FinalizePose; Remake has no such step, and its aim offset is lagged and read
// back next frame, so writing the pose would shrink it a little more each frame in a leader change.
//
// The battle camera's lean. FEndCameraFlavorTargetOffset aims by the target's screen X against the middle of the whole window,
// from a projection that counts the window's pixels. A view on the left is a quarter window off that middle, so it leans 25
// units left however centred the enemy is, and a view on the right 25 right. Its projection call is given the game's own
// `viewportRelative` argument and the X divided by the view's Size X, which puts it in the window's range as in single player;
// a view the window's width is unchanged, its X the same either way.
//
// Game thread (the operators' Ticks run there; the mod's scopes make the projection's controller the ticking player's). The
// console's `shoulder-scale on|off` and `target-lean-fix on|off` (on) are for the comparison in play; reading
// `narrow-camera.state`. Logged: Info when a player's factor changes, and once for each install.

#include "views/column_rules.h"

#include <array>

namespace lss::platform {
class GameImage;
}

namespace lss::views::narrow_camera {

/// By seat: player 1's local player, then the seats'; null for one not in.
using SeatPlayers = std::array<const void*, column_rules::kMaxColumns>;

/// Checks the two calls against the research and registers the commands. False (logged) if a call differs.
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, outside the world's tick: places the crossing call once, and reads each player's factor.
void Tick(const SeatPlayers& players) noexcept;

/// Any thread: puts the aim offset's call back, and leaves the crossing call placed, passing through (restoring it is safe
/// only from the thread that runs it, and an unload may come from another; the DLL stays pinned once a redirect is placed).
void Remove() noexcept;

}  // namespace lss::views::narrow_camera
