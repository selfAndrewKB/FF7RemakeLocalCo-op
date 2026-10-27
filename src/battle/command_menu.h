#pragma once

// Each seat's own battle command menu: a FieldCommandCtrl per seat, built and kept with the game's own calls
// (RE-command-menu-two.md; Rebirth's player_two/battle/command_menu_two, command_locks and command_menu_views in one, which
// stayed single-seat; the seats here follow plan/seats.md step 3).
//
// The game owns one command controller (EndUISystem[19]) and builds it with three plain calls: GMalloc, the
// constructor, SetupMenuWidgets(ctrl, world). Setup creates the controller's own two widgets (Command_C, the
// command menu; Status_C, the party panel), pushes its menu as a focus layer and binds its handlers, so a seat's
// controller is built the same way inside that seat's scopes (players/scopes.h): its layer lands in the seat's
// own focus context, never above player 1's menu. It follows the game's lifecycle at the game's own calls, each
// made again on every built seat's controller right after the game's: Setup (two call sites), ResetAll (three), the
// battle HUD mode (its entry), the party panel rebuild (one site inside RefreshFieldMenu), the command lists (entry),
// the menu-allowed setter (entry). Two bytes the game writes directly on its controller (menu allowed, input
// live) are copied each tick: the only values with no call to repeat. What the game did while a seat was out is caught
// up when its player is back: the battle HUD mode, and the party panel (the game's refresh after the seat's Setup).
//
// A seat's widgets stay transparent (the game's SetColorAndOpacity) except in its column's pass of the HUD render
// (hud/overlay_pass.h: a seat's column is its index), where they take player 1's roots' colours and player 1's two
// roots go transparent instead; opacity keeps the widget painted and on the focus path, which Hidden would not. The
// widget's key states (pressed, held, released) are advanced in its NativeTick, once per overlay paint, and the mod
// paints more than once: the NativeTick slot runs each command menu once per frame, in player 1's pass (the further
// passes are skipped), a seat's inside that seat's scopes.
// Game thread only; the controllers are never freed, as the game's is not.

#include "hud/overlay_pass.h"

namespace lss::platform {
class GameImage;
}

namespace lss::battle::command_menu {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick, per built seat: builds the seat's controller once its player plays a member and the game's is
/// set up; after each Setup catches up what the game did while the seat was out (the party panel, the battle HUD
/// mode); keeps the two copied bytes current.
void Tick(int seat, bool seatPlays) noexcept;

/// The seat's player leaves (or a world change takes it out): its controller's layers are released inside its own
/// context and its context forgotten; the controller waits for the seat's next player.
void OnLeave(int seat) noexcept;

/// The seat's controller, or null until built; Ready once its Setup ran.
void* Controller(int seat) noexcept;
bool  Ready(int seat) noexcept;

/// Around each pass of the HUD render (the column whose pass runs, overlay_pass::kSingle for the game's one render):
/// the column's seat's menu and panel visible, player 1's transparent; every other seat's stay transparent.
void BeginPass(int column) noexcept;
void EndPass() noexcept;

}  // namespace lss::battle::command_menu
