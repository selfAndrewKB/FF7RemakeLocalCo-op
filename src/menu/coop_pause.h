#pragma once

// The Co-op row in the battle pause (research/RE-battle-pause-coop.md; the user, 2026-11-01: the battle's rates changed
// in a fight). The stock battle pause has no settings: Resume, Retry from Last Battle, Retry from Checkpoint, Return to
// Title. The game builds its rows as data on its pause manager (a kind and a text id each) every time it opens it;
// after the game's build the mod adds a "Co-op" row of a kind the game's press dispatch ignores (10), above Return to
// Title, and sets the widget's labels again with the game's own setter. Its press takes the pause down the game's way
// (the game stays paused) and opens the Co-op page as the main menu's row does (menu/coop_page_open, as a Host); the
// page's close opens the battle pause again through the same open the game uses, so the row is there again. A moment
// that ends under the page (the game no longer in its menu state) takes the page down with no pause brought back; a
// pause that cannot be brought back resumes the game the way Resume does, so the game is never left paused with no menu.
// Logged at Info (placed, opened, closed), the row's add at Debug. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::menu::coop_pause {

/// False if a function or call site is not where the facts say.
bool Install(const platform::GameImage& image) noexcept;
/// The three call redirects (first tick, game thread). False if one could not be placed (then none is).
bool Place() noexcept;
void Unplace() noexcept;

}  // namespace lss::menu::coop_pause
