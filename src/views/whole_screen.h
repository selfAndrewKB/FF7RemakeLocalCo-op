#pragma once

// Whether the game shows something that takes the whole screen, from the game's own state: the reason the
// one-window single view collapses the split (players/single_view.h applies it; views/column_source.h decides it as
// Why::WholeScreen; the user's ruling of 2026-10-26: in one window cutscenes, menus and minigames collapse to the
// engine's single view as in Rebirth, and the mirror look stays the monitor row's). Rebirth's views/whole_screen gave
// the shape (its reads from its RE-p2-single-view.md); Remake's signals are the ones its research located
// (research/RE-lifecycle-world.md §2, §4, §8; RE-pause-menu-any-pad.md §3.4; RE-shop-two.md §3), most of them read by
// another module already, so this one reads through them:
//   - the game state (players/game_state.h): 8 a pause menu owns the screen (the main menu, the map, battle pause,
//     game over, colosseum, the script-opened pause menus), 7 the pause during a cutscene, 10 the chapter-start
//     pause; the menu's kind names which;
//   - the on/off table (players/field_root.h OnOff; Square Enix's EndFieldOnOffTable reference counts): the
//     script-opened menus 72-76; 12 ShowSkipPauseMenu (a skippable scene); 39, 40 and 53 a held black screen;
//     41 the loading screen; 36 MiniGamePlaying (which minigames hold it is the open IDA pass,
//     research/RE-minigame-signal.md);
//   - the shop UI's own "a shop is up" byte (players/game_state.h ShopUp: a shop runs in the field state) and the
//     game's own IsPauseMenuClosed (PauseWidgetAlive: a Blueprint-opened pause menu, Libra from the main menu);
//   - through views/scene_camera.h's read (ReadSceneNow; nothing of these is known while its globals failed their
//     check): the cutscene manager playing, its skip byte, and the field control lock outside player 1's own move.
//     A talk or an event camera holds every player in Remake (the whole game stops), unlike Rebirth's
//     walk-and-talks, so they collapse too. The lock counts outside a battle only (the battle AI and the boss
//     restrictions hold lock bits of their own) [ASSUMPTION until played: which other lock holders a further player
//     meets in the field, a door or a chest of player 1's among them, and whack-a-box, whose bit 1 is on around its
//     play and off in it; the play check reads them off the Debug signal line];
//   - a VR simulator or colosseum session (on/off 78 ShowColosseumMenu, held for the whole session; research/
//     RE-minigame-signal.md §1-2): its menus and results collapse (78 with the controller-exclusion bit 21 outside a
//     battle, Rebirth's arena rule; the pause state covers the rest), its fights stay split (bit 21 and 12 are not
//     counted under 78);
//   - a bike chase (chapters 4 and 18): the game's own test of its shared bike state (research/RE-minigame-signal.md
//     §4: the chases are a mode of their own, neither a cutscene nor an act with on/off 36; the VR simulator, the
//     colosseum and Fort Condor hold the controller-exclusion bit 21 of the lock mask: the scene read counts it for Fort
//     Condor and masks it out under 78, above);
//   - the title, by the field root's location name.
// Read once per engine tick before the session (Tick): logged at Info when something takes the whole screen or lets
// go, at Debug for a reason changing within a moment and for each change of the signals; Shown() answers this tick's.
// The reads patch nothing. Reading `whole-screen`. Game thread.

#include "views/whole_screen_rules.h"

namespace lss::platform {
class GameImage;
}

namespace lss::views::whole_screen {

/// Rebases the bike state's globals and registers the reading. False (logged) if a global is outside the image: the
/// bike chase is then not read; the rest is another module's.
bool Install(const platform::GameImage& image) noexcept;

/// Once per engine tick, after the game state's tick: reads the signals, logs what changed.
void Tick() noexcept;

/// This tick's reason, None when nothing takes the whole screen or nothing is readable.
Reason ReasonNow() noexcept;

/// The game shows something that takes the whole screen this tick.
bool Shown() noexcept;

/// This tick's signals.
Signals SignalsNow() noexcept;

}  // namespace lss::views::whole_screen
