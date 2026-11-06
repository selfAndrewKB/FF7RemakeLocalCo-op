# Local Split Screen for FINAL FANTASY VII REMAKE INTERGRADE

Play the whole game in co-op on one PC: up to three players, each with their own controller, character, camera, HUD and
battle menu. In one window the screen splits. With two or three monitors each player gets a full screen of their own.

## What you need

- FINAL FANTASY VII REMAKE INTERGRADE on Steam, file version 1.0.0.7. It is the only version the mod was made on. At
  every start the mod checks the game's code and leaves out, with a line in its log, whatever does not match.
- UE4SS 3.0.1, the mod loader ([RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)), set up for this game with its own
  game config "Final Fantasy 7 Remake". In its `UE4SS-settings.ini`, `HookLoadMap` must be `0`: at 1 the game crashes
  every time it closes.
- One controller for each further player: an Xbox controller, or any other that Steam shows to games as one (Steam Input).
  Player 1 plays on a controller or the keyboard.

## Installing

1. Close the game.
2. Copy the `LocalSplitScreen` folder from the zip into `<game>\End\Binaries\Win64\ue4ss\Mods\`.
3. Start the game from Steam as always.

The folder carries its own `enabled.txt`, so UE4SS's `mods.txt` needs no line for it. To switch the mod off, delete
`enabled.txt`; to remove it, delete the folder.

## Playing

- **Player 1** is whoever presses a button first on the title screen.
- **Joining.** Press **Start** on a controller nobody is playing on. Player 2 plays the second character of the party's
  order and player 3 the third, each with a view of their own, and each keeps that place when you reorder the party. If
  that character is not free at that moment (a solo stretch of the story, a character the story is moving), the player
  joins as soon as it is.
- **Your own window.** Hold **View** (the small left button) for a second. It opens in your own view: left and right on
  the d-pad change your character (players 2 and 3), LB and RB move your view to another screen, A closes it, and holding
  B for a second leaves the game. Player 1 cannot leave.
- **By keyboard**, if you need it: Ctrl+F7 brings player 2 in and Ctrl+F8 takes them out; with Shift added, the same for
  player 3.
- **Each player** walks, talks, shops, opens chests and doors, climbs and fights on their own controller, with their own
  prompts, markers, minimap, lock-on and command menu.
- **One picture for everyone.** Menus, shops, cutscenes, minigames and scripted conversations show as they do in single
  player, and the split returns when they end. A character the story takes over is handed back afterwards.
- **In battle** a knocked-out player stays on their character until revived, and enemies divide their attention among
  the players.
- **Left behind?** Players 2 and 3 can click the left stick (L3) twice. Outside a battle it brings their character back
  beside player 1's; in a battle it brings a character that was shut out of the fight's area back into it. It does
  nothing during a scene or a talk, on a ladder or in a crawl, or when the character is already beside player 1's.

## The Co-op page

The game's main menu has a **Co-op** page, and so has the title menu, below Options. It holds the mod's options: how the
screen splits for two and for three players, each view's field of view, which screen shows which player, the battle's
rates and more. Every option says what it does when you move onto it. A choice is saved at once to
`LocalSplitScreen.ini` in the mod's folder, which the mod writes itself with every option explained in it.

## Several monitors

One full view per player, each on its own monitor.

1. With the game closed, run **Multi Monitor Setup** (`MultiMonitorSetup.exe` in the mod's folder).
2. Click two or three monitors in the order you want them, left to right. Each is marked with the player it will show.
3. Set **Multiple Monitors** to On and press **Save**.
4. Start the game from Steam, in Borderless or Windowed display mode. The app need not stay open.

The monitors need not stand side by side in Windows, and they may differ in size and scaling. While the game runs they are
moved into a row, and they go back when it ends, even after a crash. Windows' own arrangement is never changed, so a
restart of the PC always returns your layout. If a game ever ends without putting them back, the app shows a **Restore
Monitors** button.

A monitor you picked that is not connected is waited for, not replaced: with two of three connected the game uses those
two, with fewer it stays in one window. A virtual monitor made by a streaming program (Apollo, Vibepollo) works like any
other and is recognised by its name, so give each device you stream to a name of its own.

Leave `MultiMonitorSetup.exe` where it is and under that name: the mod starts it in the background to put your monitors
back if the game dies.

## Known limits

- A view wider than about 4096 pixels (a 32:9 monitor) shows a band of wrong colours at its right side.
- On a row of monitors, a pre-rendered movie is cropped to fit an ultrawide monitor.
- A controller that Windows does not show as an Xbox controller is not seen as a further player's; Steam Input makes it
  one.
- Classic mode has not been tested.

## If something goes wrong

The mod writes to UE4SS's log, `<game>\End\Binaries\Win64\ue4ss\UE4SS.log`, in lines marked `[LocalSplitScreen]`. The
first says the version (`v` and its number, then `starting`); warnings say what could not be set up and what was left out
because of it. Send that file with a report.

## Thanks

Built on [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS).
