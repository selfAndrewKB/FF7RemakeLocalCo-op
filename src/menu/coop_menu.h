#pragma once

// The Co-op row of the game's main menu and the page it opens (research/RE-game-widgets.md §9-§10; plan/topics/
// input.md History 2026-10-31; the rows and texts: menu/coop_menu_rules.h; the page: menu/coop_page; its opening and
// closing: menu/coop_page_open).
//
// The row: the main menu builds its left list from four tables by row kind; the mod gives the trial's "Buy Now" kind
// (12, never shown by the full game) its text, help and an unlock flag of none, and puts it before System, so the game
// builds, draws, selects and remembers the row itself. A press of it reaches the game's window opener, where the mod's
// redirect opens the page instead of the trial's action. The page is the game's Options screen with the settings
// table's options, driven by the game's list, value popup, help line, sounds, animations and focus layer, so every pad
// does under the menu-time rule; a change applies at once (for this session; the file is not written).
// The title menu has a row of its own to the same page (menu/title_row), placed with these or left out by its own checks.
// Install checks every part's functions (menu/game_ui, menu/coop_page, menu/coop_page_open) or places nothing. The
// redirects and the table words are placed on the first tick (game thread), the row's tables last, so the row never
// shows without its opener. Each tick runs the page's queued changes, then watches the open page's close. Reading
// `menu.coop`; the dev switch `menu.coop-page-hidden`. Every step logged at Info. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::menu::coop_menu {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once per engine tick on the game thread.
void Tick() noexcept;

}  // namespace lss::menu::coop_menu
