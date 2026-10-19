#pragma once

// Player 2's own battle command menu: a second FieldCommandCtrl built and kept with the game's own calls
// (RE-command-menu-two.md; Rebirth's player_two/command_menu_two, command_locks and command_menu_views in one).
//
// The game owns one command controller (EndUISystem[19]) and builds it with three plain calls: GMalloc, the
// constructor, SetupMenuWidgets(ctrl, world). Setup creates the controller's own two widgets (Command_C, the
// command menu; Status_C, the party panel), pushes its menu as a focus layer and binds its handlers, so player 2's
// controller is built the same way inside player 2's scopes (players/scopes.h): its layer lands in player 2's
// own focus context, never above player 1's menu. It follows the game's lifecycle at the game's own calls, each
// made again on player 2's controller right after the game's: Setup (two call sites), ResetAll (three), the battle
// HUD mode (its entry), the party panel rebuild (one site inside RefreshFieldMenu), the command lists (entry),
// the menu-allowed setter (entry). Two bytes the game writes directly on its controller (menu allowed, input
// live) are copied each tick: the only values with no call to repeat.
//
// Its widgets stay transparent (the game's SetColorAndOpacity) except in player 2's pass of the HUD render,
// where player 1's two roots go transparent instead; opacity keeps the widget painted and on the focus path,
// which Hidden would not. The widget's key states (pressed, held, released) are advanced in its NativeTick,
// once per overlay paint, and the mod paints twice: the NativeTick slot runs each command menu once per frame,
// in player 1's pass (player 2's pass is skipped), player 2's inside player 2's scopes.
// Game thread only; the controller is never freed, as the game's is not.

#include "hud/overlay_pass.h"

namespace lss::platform {
class GameImage;
}

namespace lss::battle::command_menu_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick: builds player 2's controller once player 2 plays a member and the game's is set up; keeps
/// the two copied bytes current.
void Tick(bool playerTwoPlays) noexcept;

/// Player 2 leaves (or a world change takes it out): its controller's layers are released inside its own
/// context and its context forgotten; the controller waits for the next player 2.
void OnLeave() noexcept;

/// Player 2's controller, or null until built; Ready once its Setup ran.
void* Controller() noexcept;
bool  Ready() noexcept;

/// Around each pass of the HUD render: the pass's own menu and panel visible, the other player's transparent.
void BeginPass(hud::overlay_pass::Pass pass) noexcept;
void EndPass() noexcept;

}  // namespace lss::battle::command_menu_two
