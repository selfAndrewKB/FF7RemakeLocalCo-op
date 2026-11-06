#pragma once

// Whether the game shows something that takes the whole screen, from the game's own state: the reason the
// one-window single view collapses the split (players/single_view.h applies it; views/column_source.h decides it as
// Why::WholeScreen; the user's ruling of 2026-10-26: in one window cutscenes, menus and minigames collapse to the
// engine's single view as in Rebirth, and the mirror look stays the monitor row's; 2026-10-27: those three, plus the
// scripted talks and events that take the game's event camera, and no more, so a free talk or a field pickup, which
// hold the field control lock only, play on every view as they did before v0.109.0). Rebirth's views/whole_screen gave
// the shape (its reads from its RE-p2-single-view.md); Remake's signals are the ones its research located
// (research/RE-lifecycle-world.md §2, §4, §8; RE-pause-menu-any-pad.md §3.4; RE-shop-two.md §3), most of them read by
// another module already, so this one reads through them:
//   - the game state (players/game_state.h): 8 a pause menu owns the screen (the main menu, the map, battle pause,
//     game over, colosseum, the script-opened pause menus), 7 the pause during a cutscene, 10 the chapter-start
//     pause; the menu's kind names which;
//   - the on/off table (players/field_root.h OnOff; Square Enix's EndFieldOnOffTable reference counts): the
//     script-opened menus 72-76; 12 ShowSkipPauseMenu (a skippable scene); 39, 40 and 53 a held black screen;
//     41 the loading screen; 36 MiniGamePlaying with its owner, the act's name (an act's exclusion row sets it from
//     data, Fort Condor natively, research/RE-minigame-signal.md §1; the Reactor 5 lock segment's acts set it, live
//     2026-10-27), the moment held over the one-tick gap between a segment's chained acts (whole_screen_rules.h
//     BridgeActGap, the mod's own bridge);
//   - the shop UI's own "a shop is up" byte (players/game_state.h ShopUp: a shop runs in the field state) and the
//     game's own IsPauseMenuClosed (PauseWidgetAlive: a Blueprint-opened pause menu, Libra from the main menu);
//   - through views/scene_camera.h's read (ReadSceneNow; nothing of these is known while its globals failed their
//     check): the cutscene manager playing, its skip byte, and the lock mask's bit 21 for the arena rule below. The
//     lock itself is no reason (v0.109.0 read it as one: a talk, a shop's talk, an event, a pickup and a one-frame
//     transition collapsed the window in the user's play 2026-10-27 01:56-02:05, bits 0, 2, 20 and 1; the user's
//     call: menus, cutscenes and minigames only, widened the same night to the event camera, below);
//   - the event camera (views/scene_camera.h SlotActive: the event camera system's slot 18 active on player 1's
//     manager, research/RE-cutscene-camera-two.md §2.3) while a scene holds the players (the field lock outside
//     player 1's own move, or the cutscene: a field area's rail camera writes 18 in free play too and holds no lock
//     [ASSUMPTION until a rail area is played; §11 Q3]), outside a battle: a scripted talk runs on it with the game's
//     own angles and cuts, the HUD off and the speakers alone in frame, and the user wants those single (2026-10-27
//     ~03:00: "almost like cutscenes, only difference is they cannot be skipped"); a free talk keeps the field
//     camera's slot 24 and stays split (live: Chadley's scripted talk on 18, the armed leader's free talk on 24; the
//     talk camera's slot 14, a look-at a shop uses, is not counted [ASSUMPTION: no free talk seen on 14]);
//   - a VR simulator or colosseum session (on/off 78 ShowColosseumMenu, held for the whole session; research/
//     RE-minigame-signal.md §1-2): its menus and results collapse (78 with the controller-exclusion bit 21 outside a
//     battle, Rebirth's arena rule; the pause state covers the rest), its fights stay split (12 is not counted under 78,
//     and bit 21 counts only outside a battle);
//   - a bike chase (chapters 4 and 18): the game's own test of its shared bike state (research/RE-minigame-signal.md
//     §4: the chases are a mode of their own, neither a cutscene nor an act with on/off 36; Fort Condor holds the
//     controller-exclusion bit 21 for its whole act and 36 with it);
//   - the title, by the field root's location name.
// Read once per engine tick before the session (Tick): logged at Info when something takes the whole screen or lets
// go, at Debug for a reason changing within a moment, for the hold over a gap between acts, and for each change of
// the signals; Shown() answers this tick's.
// The reads patch nothing. Reading `whole-screen`. Game thread.

#include "views/whole_screen_rules.h"

namespace lss::platform {
class GameImage;
}

namespace lss::views::whole_screen {

/// Rebases the bike state's globals and registers the reading. False (logged) if a global is outside the image: the
/// bike chase is then not read; the rest is another module's.
bool Install(const platform::GameImage& image) noexcept;

/// Once per engine tick, after the game state's tick: reads the signals (the event camera's slot on player 1's
/// camera manager, null when player 1 has none), logs what changed.
void Tick(const void* managerOne) noexcept;

/// The game shows something that takes the whole screen this tick.
bool Shown() noexcept;

}  // namespace lss::views::whole_screen
