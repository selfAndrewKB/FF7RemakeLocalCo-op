#pragma once

// Each player's view takes the indoor or outdoor look of its own camera (the user's report of 2026-10-25: player 1's
// Cloud walks into a building, the picture brightens for Cloud, and the outside turns very bright in the other players'
// views; research/RE-indoor-light-two.md. Rebirth has no counterpart).
//
// Square Enix places area controllers in each level's lighting sublevel (camera exposure, auto exposure, tonemap, fog,
// lights). Those of the "time in box / time in sphere" kinds fade their settings in over a blend time while the camera
// is inside the area and out after it leaves, with one fade timer per controller: each frame's time is added to a
// pending value by the controller's tick, and the first view evaluated in the frame (player 1's) moves the timer by it
// and zeroes it. Every later view moved the timer by nothing and took player 1's fade level, wherever its own camera
// was; a further player alone inside never faded in.
//
// While more than one view is drawn, around each view's evaluation of the game's blend (the call the mod already
// redirects for shadows per view) the mod puts that view's own timers into the controllers, and for the frame's later
// views the frame's pending time and snap byte as the first view found them, then lets the game's function run
// unchanged, and keeps the timers it leaves as that view's. So each view fades in and out over the game's own blend
// time from its own camera, a camera cut snaps its own view only, and the arrival snap of a field step reaches every
// view. With one view nothing is touched (when the split ends, the controllers the world lists then get the first
// view's timers back). The timers are kept per view state (one per local player), seeded from the first view's, and
// dropped at a world change or when the view stops being drawn. Switch `area-blend` (on: each view its own; off: the
// game's one timer, as before).
// Logged: Info when the per-view timers start and stop; Debug at a world change; the `render.area-blend` reading (the
// timed controllers, each with its kind, blend time and every view's fade).

namespace lss::platform {
class GameImage;
}

namespace lss::render::area_blend {

/// Registers the switch and the reading (the site is shadows_per_view's call redirect).
void Install(const platform::GameImage& image) noexcept;

/// Game thread, once per engine tick, before the frame's views: whether more than one view is drawn.
void Tick(bool severalViews) noexcept;

/// Game thread, around the game's blend for one player view (its FSceneView and its view state).
void Before(const void* view, const void* state) noexcept;
void After(const void* view, const void* state) noexcept;

}  // namespace lss::render::area_blend
