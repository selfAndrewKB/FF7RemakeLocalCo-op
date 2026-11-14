#pragma once

// The low-health red edge in each player's own view. Left alone, one member at a quarter of their HP or below lights the
// edge in every view: the game decides once for the whole party and draws one post-process effect every view reads
// (research/RE-low-health-effect.md).
//
// The decision, by the game's own function: each command controller's tick runs the game's danger check, which reads
// each member's HP through one call. While a seat's controller ticks, that call answers only for the seat's own member
// (every other member reads as never low); while player 1's ticks, for every member no seat holds, so player 1's view
// also takes an AI member's danger, as in single player. Each controller's own flag then says "my player is in danger",
// and the game's shared count keeps the one effect up while any is.
//
// The fade, by the game's own code: the game fades the effect in and out with a linear fade it runs every HUD tick on
// the screen-filter slot's holder. Each player gets a zeroed holder of their own, set going the way the game's setter
// sets its own (target, from the current value, over the game's length) whenever their flag changes, and run by the
// game's same tick function right after the game's own, on the same frame time.
//
// The view: around each player view's post-process blend (the call render/shadows_per_view redirects), the component's
// settings copy takes that player's own fade, never above the game's value, and its narrow intensity bit is cleared at
// zero; both are put back after the call. So each view fades on its own player's edges, and every gate of the game's
// (a pause menu's fade-out and its return) still reaches every view through the cap. With one view nothing is touched.
//
// An event that hides the effect writes player 1's controller only; a seat's fade follows player 1's allowed byte.
// Switch `low-health-per-view` (on: each view its own; off: the game's whole-party edge in every view). Logged: Info on
// each player's danger edge; Debug when a view first differs from the game's value and when it matches again; the
// `render.low-health` reading.
// Game thread only.

namespace lss::platform {
class GameImage;
}
namespace lss::views::columns {
struct Map;
}

namespace lss::render::low_health {

/// Checks the two calls and the function against the research, places the redirects, registers the switch and reading.
/// False (logged) if a check fails; nothing is placed then.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once per engine tick, before the frame's views: the columns as the engine lists them.
void Tick(const views::columns::Map& map) noexcept;

/// Around the game's blend for one player view (its FSceneView).
void Before(const void* view) noexcept;
void After() noexcept;

}  // namespace lss::render::low_health
