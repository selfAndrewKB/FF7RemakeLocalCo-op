#pragma once

// The Co-op row of the game's main menu and the page it opens (research/RE-game-widgets.md §9-§10; plan/topics/
// input.md History 2026-10-31; the rows and texts: menu/coop_menu_rules.h).
//
// The row: the main menu builds its left list from four tables by row kind; the mod gives the trial's "Buy Now" kind
// (12, never shown by the full game) its text, help and an unlock flag of none, and puts it before System, so the game
// builds, draws, selects and remembers the row itself. A press of it reaches the game's window opener, where the mod's
// redirect opens the page instead of the trial's action.
// The page: the mod's own instance of the game's Options screen (not the one System keeps), its categories and rows
// replaced with the settings table's options after the game's construct (which runs at each open: a closed page takes
// itself out of the overlay), then opened the game's way. Its value reads and commits are the game's two functions,
// redirected for the mod's rows only (their category bytes from 0x80, which the game never makes): a commit is queued
// and the next tick runs the option's console command, so it applies at once (for this session; the file is not
// written). The game's list, value popup, help line, sounds, animations and focus layer drive it, so every pad does
// under the menu-time rule. While it is open the main menu's current window stays the Top window (hidden), its title
// reads "Co-op" and its guide the page's keys, and the game's heading table (keyed by category, keys the page shares
// with the game's first three) names the page's categories. Back on the page's categories closes it in the same input
// event (the page's key handler, a vtable slot) and the Top window comes back as after any game window. A main menu
// closed under it (no game path is known to) takes the page down the game's no-animation way.
// Redirects and the table words are placed on the first tick (game thread), the row's tables last, so the row never
// shows without its opener. Every step logged at Info, each commit at Debug, each value read at Trace. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::menu::coop_menu {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Once per engine tick on the game thread.
void Tick() noexcept;

}  // namespace lss::menu::coop_menu
