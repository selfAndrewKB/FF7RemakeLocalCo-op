# LocalSplitScreen (FF7 Remake)

Two-player split-screen for FINAL FANTASY VII REMAKE (Intergrade, UE 4.18), as a UE4SS C++ mod. Plan and research:
`../Documentation/PLAN-REMAKE-COOP.md` and `../Documentation/research/RE-*.md`.

## Keys

| Keys | What |
|------|------|
| Ctrl+F7 | Player 2 joins: own End player controller, camera manager and field camera (following Aerith) |
| Ctrl+F8 | Player 2 leaves |
| Ctrl+F9 | PROBE: player 2 takes Aerith through the game's leader funnel, or gives her back (borrows player 1's PlayerTrigger) |
| Ctrl+F6 | Log every local player's controller, pawn, camera manager and camera actor |

While player 2 is in, each player's sticks are their own pad's (pad 1 = XInput user 0, also the keyboard;
pad 2 = user 1).

## Building

Its own CMake root; builds RE-UE4SS from `../../Rebirth/_tools/UE4SS_CppMods/RE-UE4SS` (override with `-DUE4SS_ROOT`).

```
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Game__Shipping__Win64 --target LocalSplitScreen
```

## Installing

UE4SS v3.0.1 (the build Rebirth uses) with the RE-UE4SS custom config `Final Fantasy 7 Remake`, and
**`HookLoadMap = 0`** in `UE4SS-settings.ini` (with it on, every exit crashes). Copy
`build/Game__Shipping__Win64/LocalSplitScreen.dll` to `<game>/End/Binaries/Win64/ue4ss/Mods/LocalSplitScreen/dlls/main.dll`
and add `LocalSplitScreen : 1` to `Mods/mods.txt`. Close the game first.

## Layout

```
src/
  mod.cpp                 UE4SS entry point, keys on the engine tick
  engine_facts.h          every address, offset and expected byte string, with its research doc
  platform/               image, verified functions and vtable slots, guards, log (shared design with Rebirth's)
  players/
    session               join and leave through UE's local-player path
    own_controller        the End camera manager's BeginPlay, made safe for a second player
    player_two_camera     player 2's field camera operator and the per-player scopes
    party_seat            PROBE: player 2 possesses Aerith
```
