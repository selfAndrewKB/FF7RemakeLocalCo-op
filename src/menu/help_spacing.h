#pragma once

// The help bar under the Co-op page, spaced for the page's long help lines: a blank line there (the gap that keeps a
// help's later lines clear of the page's bottom border) is half a line, not a whole one. The bar is one UEndTextBlock
// shared by every main-menu page and the title's Options (research/RE-options-help-bar.md). Its line height is a
// reflected float with no setter, so the page writes it and calls the block's own SynchronizeProperties, and puts the
// game's value back the same way when the page closes (menu/coop_page SetOpen). Logged at Info. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::menu::help_spacing {

/// Finds the text block's SynchronizeProperties. False (logged) if the check failed: the help bar keeps the game's
/// spacing.
bool Install(const platform::GameImage& image) noexcept;

/// The page opened in `menu`'s frame (a UEndMainMenu, whose help bar it shows): the bar spaced for the page.
void Tighten(void* menu) noexcept;
/// The page closed: the bar's own line height back, if the bar is still alive.
void Restore() noexcept;

}  // namespace lss::menu::help_spacing
