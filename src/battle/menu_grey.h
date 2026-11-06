#pragma once

// The open battle menu's grey and sound muffle follow the menu slow-down switch: with the slow off, a grey screen and a
// muffled mix under a world that runs on would be all that is left of the wait (research/RE-menu-grey.md).
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
//   - switched off while a battle menu waits, the effect stops later in the same engine tick (the keys and the
//     console run first; this module follows MenuSlow() at its tick, so no callback between the two modules);
//     switched on, a counted, unsuppressed filter with no effect up is started again as the game's own unsuppress
//     does.
// The filter's count is shared with other owners (a minigame's wait, a Blueprint's show): a skip or a stop can leave
// their count with no effect. Each skip or stop leaves a debt; once no battle menu waits, the engine tick settles it
// the game's unsuppress way (counted, unsuppressed, none up: started). With the switch on, every call is the game's.
// Logged: Info at install (a Warning when a restore site is missing) and through the slow-down's own switch line,
// Debug per skipped spawn, skipped restore, switch-time stop or start and settled debt; the reading `menu-grey`.
// Known edge: with `command-menu` switched off mid-menu a further player's controller stops ticking at an open wait, so
// restores stay skipped and a debt waits until that player leaves (the leave ends the wait the game's way).
//
// The muffle follows the same switch (research/RE-menu-sound-muffle.md). It is the game's WaitMode
// sound event, raised and lifted by the wait's own two calls for the one mix, whichever player's menu it is. It is not tied to
// the slow-down in the game, so the switch has to reach it here. Switched off: both calls are skipped (the game's other sites,
// a summon's own muffle among them, stay as they are; an unmatched lift from them is harmless, the values being absolute).
// Switched on: the game's, except that a close leaves the muffle up while another player's menu still waits, or the first of
// two to close would lift it for the other (Rebirth's rule). A flip with a menu waiting runs the game's own call once. The
// reading `menu-sound` shows what the mix holds. Logged at Info at install, Debug per skip, hold and flip.
// Known edge, switched on only: a further player's controller that stops ticking at an open wait (`command-menu` off mid-menu)
// still counts as waiting, so the muffle stays until that player leaves or its menu ticks again.
//
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
