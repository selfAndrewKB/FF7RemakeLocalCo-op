#pragma once

// The help bar under the Co-op page, fitted to the page's long help lines (research/RE-options-help-bar.md). The bar is
// one UEndTextBlock shared by every main-menu page and the title's Options; its text grows down from a fixed top, so a
// help of several lines would run past the page's bottom border.
// - Spacing: a blank line in a help (the gap under its first line) is half a line, not a whole one. The line height is
//   a reflected float with no setter: the page writes it and calls the block's own SynchronizeProperties.
// - Lift: while a help taller than one line shows, the bar stands up by the difference, so its last line ends where a
//   one-line help's does, inside the border. The bar's canvas slot is moved by its reflected SetPosition, the setter the
//   game uses (menu/coop_slider PlaceCanvasSlot); the height is the text's own desired height, rebuilt with its text.
// Both are put back the same way when the page closes (menu/coop_page SetOpen). Logged at Info (the open and close) and
// Debug (each lift). Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::menu::help_spacing {

/// Finds the text block's SynchronizeProperties. False (logged) if the check failed: the help bar keeps the game's
/// spacing.
bool Install(const platform::GameImage& image) noexcept;

/// The page opened in `menu`'s frame (a UEndMainMenu, whose help bar it shows): the bar spaced for the page.
void Tighten(void* menu) noexcept;
/// Each engine tick while the page is open: the bar lifted by the shown help's extra height, when that changes.
void Tick() noexcept;
/// The page closed: the bar's own line height and place back, if the bar is still alive.
void Restore() noexcept;

}  // namespace lss::menu::help_spacing
