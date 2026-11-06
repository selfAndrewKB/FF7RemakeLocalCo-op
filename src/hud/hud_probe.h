#pragma once

// Logging only: what the HUD is made of right now, for the HUD research and for checking each HUD step live.
// Nothing here changes the game.
//
//   hud.dump [roots|shown|all] [<text>]   one log line per live UUserWidget (from hud/user_widgets.h): class,
//                                         name, root or child, ZOrder, whose PlayerContext (p1/p2/none),
//                                         visibility, ColorAndOpacity alpha, viewport position, size, anchors and
//                                         alignment. `roots` (the default) lists the widgets on SEndRootOverlay,
//                                         `shown` the roots that are not collapsed, hidden or transparent, `all`
//                                         every user widget; <text> keeps the lines whose class or name holds it.
//   hud.overlay                           reading: the overlay's layout fields (RE-hud-split.md): the UI
//                                         rect's Min and size, the marker host origin, size and scale, and the
//                                         frame its last render ran in.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::hud_probe {

/// Rebases the overlay global, starts the widget list and registers the commands. False (logged) if the widget
/// list cannot be kept (the dump then walks every object each time).
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::hud::hud_probe
