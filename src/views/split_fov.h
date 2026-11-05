#pragma once

// A view wider than the game's 16:9 image (two players top and bottom, the top view of three) keeps a fuller picture, and a
// full-height narrower one (two players side by side, the large view of three down one side) the characters' size: the split
// FOV rules (views/split_fov_rules.h) written into the view point each time the engine asks for it.
//
// The game has no aspect, ultrawide or FOV option, and its own FOV path (the operator's setting requests) carries an
// absolute FOV, covers only the follow camera and cannot know the view's shape (research/RE-view-size-fov.md §4.4), so
// the rule is a documented recreation at the narrowest place: ULocalPlayer::GetViewPoint (vtable slot 72), whose result
// both GetProjectionData and CalcSceneView read (twice per player per frame). The replacement calls the engine's
// function, then rewrites the FOV (+0x18) when every condition holds:
//   - the view is not held to its camera's aspect (bConstrainAspectRatio, +0x2C bit 0, clear: the mod's fill);
//   - the player's AspectRatioAxisConstraint (LP+0x7C) is MaintainXFOV, so the FOV is read as the horizontal one (the
//     config's value; 4.18 has no scene override of it: RE-layout-fov §3.2). The mod never writes it; any other value
//     leaves the view as the engine made it, logged once;
//   - a rule applies to the view's shape: wider than the camera's image and shorter than the single-player picture, or
//     narrower than the camera's image at the single-player picture's full height.
// The view's shape is read on each call from the same values GetProjectionData uses (the player's Size fractions times
// the viewport's pixels), so a window drag or a layout change is followed in the same frame. Every camera's FOV (the
// follow camera's rows, battle, scenes, notify overrides) passes through, so all are reshaped alike. Rebirth's
// views/player_view_point did the same with a weight of 0.5; Remake keeps the formula, reads the shape per call
// instead of polling, measures against the single-player picture instead of the screen, never writes the axis rule
// (Rebirth had to win it back from scene cuts) and leaves 16:9 views alone.
// Threads: the engine calls it on the game thread (Draw's CalcSceneView, GetProjectionData's callers); the FOV write
// touches only the caller's own result and reads atomics and guarded memory, so it is safe on any thread; the log
// records are kept on the game thread only. Keep it that way: the replacement must not write shared state.
// What reaches the new FOV: everything that projects through GetProjectionData (the drawn view, its culling, the stock
// world-to-screen projections). Code that projects from the camera manager's cached view FOV does not see it (the
// animation judge's snapshot; any marker placement off that path): live check in research/RE-view-size-fov.md §8 L6.
// Always on. `split-fov.horizontal` (0.50) and `split-fov.vertical` (1.00), 0.00 to 1.00 in steps of 0.05, from the settings
// table, reading `split-fov.state`. Logged: Info when a view starts or stops taking the rule (its rect and the factor), once per refused
// axis rule; Trace per call with the FOV before and after.

namespace lss::platform {
class GameImage;
}

namespace lss::views::split_fov {

/// Replaces the local player's GetViewPoint slot and registers the commands. False (logged) if the slot or the engine's
/// function differs from the research: the views keep the engine's FOV.
bool Install(const platform::GameImage& image) noexcept;

/// Puts the engine's function back (unload; any thread: one aligned pointer).
void Remove() noexcept;

/// The factor on the camera's sideways offset in this local player's view at the vertical split's weight
/// (split_fov_rules::ShoulderFactor); 1 when its shape cannot be read or the views keep the engine's FOV. Game thread.
float ShoulderFactor(const void* localPlayer) noexcept;

}  // namespace lss::views::split_fov
