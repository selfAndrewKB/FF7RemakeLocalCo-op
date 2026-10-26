#pragma once

// The open battle menu's grey follows the menu slow-down switch (the user, 2026-10-26: the switch "revealed the need to
// also disable the greying of the screen"; research/RE-menu-grey.md).
//
// The grey is the game's wait tint, UI filter type 0: a particle effect the UI filter manager (EndUISystem[37]) spawns
// when a command controller's wait state opens and the filter's shared count goes from 0 to 1, and stops when the
// count returns to 0 at the wait's close. One spawn site serves every player's menu (further players' controllers tick
// the same code in their scopes); the effect is global, so any open menu greys every view.
//
// With the slow-down switched off (battle/command_menu_tick.h, Ctrl+F9, `menu-slow`, `[Battle] menu-slow`):
//   - the wait's spawn call is skipped (a call redirect); the game still takes its latch and count, and its own close
//     drops them and finds no effect to stop (the handle stays empty), so nothing leaks or sticks;
//   - the four calls that bring a counted filter back after a menu closes over it (the top/pause menu, menu widgets A
//     and B, and EndUIFilter__Unsuppress for the others) skip type 0 while a battle menu waits (a controller's wait
//     state opening or open), so no grey returns there;
//   - switched off while a battle menu waits, the effect stops at once; switched on, a counted, unsuppressed filter
//     with no effect up is started again as the game's own unsuppress does.
// The filter's count is shared with other owners (a minigame's wait, a Blueprint's show): a skip or a stop can leave
// their count with no effect. Each skip or stop leaves a debt; once no battle menu waits, the engine tick settles it
// the game's unsuppress way (counted, unsuppressed, none up: started). With the switch on, every call is the game's.
// Logged: Info at install (a Warning when a restore site is missing) and through the slow-down's own switch line,
// Debug per skipped spawn, skipped restore, switch-time stop or start and settled debt; the reading `menu-grey`.
// Known edge: with `menu-two` switched off mid-menu a further player's controller stops ticking at an open wait, so
// restores stay skipped and a debt waits until that player leaves (the leave ends the wait the game's way).
// Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::menu_grey {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread, each engine tick, outside every scope: notes player 1's command controller, settles a debt.
void Tick() noexcept;

}  // namespace lss::battle::menu_grey
