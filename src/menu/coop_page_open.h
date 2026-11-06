#pragma once

// Opening and closing the Co-op page over the main menu, as the game shows and closes its own windows
// (research/RE-game-widgets.md; the page: menu/coop_page; the row that opens it: menu/coop_menu).
//
// An open runs the game's window opener's tutorial gate and prelude, then hides the main menu's first page (the Top
// window) and opens the page over it the game's way; the manager's current window stays the Top window, so its title,
// Back and re-open paths stay the game's. While open the Top window's title reads "Co-op" and its guide the page's keys,
// and the game's heading table (keyed by category, keys the page shares with the game's first three) names the page's
// categories. Back on the page's categories closes it in the same input event (the page's key handler, a vtable slot)
// and the Top window comes back as after any game window; a close the handler did not see is caught by the tick. A main
// menu closed under the page (no game path is known to) takes it down the game's no-animation way. While open, the
// hidden Top window is held from the garbage collector (the engine's root set, as AddToRoot), since its Hide takes it out
// of the overlay and nothing else keeps it: collected, the manager's tick would empty the title, help and guide.
// Every close lets it go. Each open and close logged at Info, the hold at Debug. Game thread.

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::menu::coop_page_open {

/// The game's window opener (its prelude kind shows nothing, then the page opens).
using OpenWindowFn = void (*)(void* mgr, std::uint8_t kind);

/// False if a function or table is not where the facts say.
bool Install(const platform::GameImage& image) noexcept;
/// The page's key handler replaced (first tick, game thread). False if it could not be.
bool Place(const platform::GameImage& image) noexcept;
/// The headings and the key handler back.
void Unplace() noexcept;
/// The unload: no UI calls (it may run off the game thread); a page open then is left to the game.
void Remove() noexcept;

/// The row's press: the page opened over the main menu manager `mgr`'s hidden first page. `openWindow` is the game's
/// window opener, for its prelude.
void Open(void* mgr, OpenWindowFn openWindow) noexcept;

/// The title menu's functions (menu/title_row). False if one is not where the facts say; the title then opens nothing.
bool InstallTitle(const platform::GameImage& image) noexcept;

/// The title row's press: the page opened over the start menu through the game's own Options open, its frame, close
/// function and stick mode with it, the mod's page standing in the start menu's page field for that one call. No first
/// page to hide or hold there: the start menu stays underneath and focus returns to it when the page's layer pops.
void OpenAtTitle(void* start) noexcept;

/// Once per engine tick on the game thread, after menu/coop_page's: a close the key handler did not see, a page gone,
/// or the menu it opened over closed under it.
void Tick() noexcept;

/// Pages opened since the start.
std::uint64_t Opens() noexcept;
/// While open, the main menu's title, help and guide as drawn and the page's visibility ("; main menu title ..."); else
/// empty. On a first open after launch they can be blank.
std::wstring FrameReading();

/// The dev switch: the open page hidden or shown as before, to tell what it covers. Empty when done, else why not.
std::wstring SetPageHidden(bool hide);
bool         PageHidden() noexcept;

}  // namespace lss::menu::coop_page_open
