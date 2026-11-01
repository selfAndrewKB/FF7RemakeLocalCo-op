#pragma once

// The Co-op page's sliders (research/RE-options-gauge-scroll.md; the user, 2026-11-01: the 100-value lists ran past the
// screen's bottom, and a slider of the game's own was asked to scroll). A row with more values than the game's longest
// list is the game's Range row (menu/coop_page builds it; coop_menu_rules::IsSlider). The game's gauge then holds one
// cell per value and grows past its frame, since nothing sizes its strip; the strip is the game's scrolling list
// (virtualising, clipped to its bounds), so after each of the game's own gauge updates (the press and every Left/Right
// step, both calls redirected) the mod sizes the strip to the stock 10 cells with the call the game's shop list makes
// (SetDesiredCount), scrolls it to keep the marker in view (SetScrollOffset, the nearest-edge rule of
// coop_menu_rules::FirstVisible), places the marker by the same canvas-slot setter the game uses, and writes the value's
// word in the gauge's label. The row's own value text is the word too (the row text's FromInt call redirected for the
// mod's sliders). The mod deciding when to scroll is the one step the game has no path for (no gauge of the game's
// holds more than its frame); the user approved it 2026-11-01. Holding Left/Right steps on (the user, 2026-11-01): the
// input already sends a held button's repeats (0.2 s, then every 0.1 s) and the page's action handler drops them at one
// test (research/RE-options-gauge-repeat.md); its vtable slot is replaced, and on the mod's open slider a held d-pad
// Left/Right reaches it as a press, the event put back after. Only the mod's page is touched. Each open logged at Debug
// with the strip's readings, each step at Trace. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::menu::coop_slider {

/// Finds the gauge's function, the row text's FromInt and the reflected calls. False if one is missing (the long rows
/// then stay lists).
bool Install(const platform::GameImage& image) noexcept;
/// The three call redirects (first tick, game thread). False if one could not be placed (then none is). Then the page's
/// action handler for held steps, on its own (logged; without it each press steps once).
bool Place(const platform::GameImage& image) noexcept;
void Unplace() noexcept;

/// The sliders are placed: the page builds its long rows as sliders.
bool Ready() noexcept;

}  // namespace lss::menu::coop_slider
