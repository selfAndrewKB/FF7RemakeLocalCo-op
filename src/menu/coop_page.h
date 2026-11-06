#pragma once

// The Co-op page itself: the mod's own instance of the game's Options screen, filled with the settings table's options,
// and its value reads and commits (research/RE-game-widgets.md; the rows and texts: menu/coop_menu_rules.h; opened
// and closed by menu/coop_page_open).
//
// The page is the mod's own (not the one System keeps), made at the first open and kept while alive. A closed page takes
// itself out of the overlay, so each open adds it again and its construct runs again over the game's data; the mod's
// categories, rows and left list then replace the game's through the game's own helpers (row info constructor, move,
// array growths, the scroll box's count and refresh), each element counted only once built. A fault while writing marks
// the page failed: the row opens nothing until the game restarts. The game's value read and commit are redirected for the
// mod's rows only (their category bytes from 0x80, which the game never makes): a read answers the option's current
// word's place, a commit while the page is open is queued and the next tick runs the option's console command, so it
// applies at once (for this session; the file is not written). Each made page and each change logged at Info, each commit
// at Debug, each value read at Trace. Game thread.

#include "menu/coop_menu_rules.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace lss::platform {
class GameImage;
}

namespace lss::menu::coop_page {

/// Finds the page's functions and builds its categories. False if a check failed or the table has no options.
bool Install(const platform::GameImage& image) noexcept;
/// The value read and commit redirected (first tick, game thread). False if either could not be placed.
bool Place() noexcept;
void Unplace() noexcept;

/// The page's categories and rows, in order.
const std::vector<coop_menu_rules::Category>& Categories() noexcept;

/// The mod's page, made if needed (from `host`'s world, with the game's `inGame`: 1 from the main menu, 0 from the title), in
/// the root overlay at `z` and holding the mod's data; null (logged) if it could not be made, added or written. A page made
/// for the other kind of host is forgotten and a new one made: the title's page carries the title's close function.
void* Ready(void* host, std::int32_t z, std::uint8_t inGame) noexcept;
/// The mod's page while alive, else null.
void* Page() noexcept;
/// `object` is the mod's page (by its weak pointer: a dead page's address may hold another object).
bool IsPage(const void* object) noexcept;
/// The option whose row the page's value read last answered, if it answered `value`; taken once (game thread: a
/// slider row's text is made right after its value read, menu/coop_slider).
std::optional<int> TakeAnswered(std::int32_t value) noexcept;

/// The page is open, over the main menu's hidden first page or the title menu (set by menu/coop_page_open): commits are
/// taken only then.
bool IsOpen() noexcept;
void SetOpen(bool open) noexcept;

/// Writing the page failed once: the row opens nothing until the game restarts.
bool Failed() noexcept;
/// Changes run since the start.
std::uint64_t Commits() noexcept;

/// Once per engine tick on the game thread: forgets the data of a page that is gone, then runs the queued commits.
void Tick() noexcept;

}  // namespace lss::menu::coop_page
