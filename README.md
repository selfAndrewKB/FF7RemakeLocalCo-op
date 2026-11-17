# LocalSplitScreen

Local split-screen co-op for up to three players in FINAL FANTASY VII REMAKE INTERGRADE, as a UE4SS C++ mod.

## Requirements

- FINAL FANTASY VII REMAKE INTERGRADE on Steam.
- UE4SS 4.0.0 (`UE4SS_v4.0.0.zip` from the RE-UE4SS releases on GitHub) with its custom game config for
  `Final Fantasy 7 Remake`. The mod is built against that release's headers and runs on no other UE4SS (an older one
  fails to load it with error 0x7f).
- To build: Visual Studio 2022 with the C++ desktop workload, CMake 3.22 or later, a Rust toolchain (UE4SS builds part
  of itself with it), Python 3.11 or later (a test and the tools), and a GitHub account linked to Epic Games (UE4SS
  fetches Unreal's headers from Epic's repository).

## Installing

1. Unpack UE4SS into `<game>\End\Binaries\Win64\`: `dwmapi.dll` beside the game's exe, and the `ue4ss` folder. Copy
   everything in the custom config's folder (`assets\CustomGameConfigs\Final Fantasy 7 Remake` in the RE-UE4SS
   repository: two layout files, the `UE4SS_Signatures` folder and a `UE4SS-settings.ini` set for the game's engine
   version) into `ue4ss\`.
2. In `ue4ss\UE4SS-settings.ini`, set `HookLoadMap = 0` under `[Hooks]`: with it on, every exit crashes.
3. With the game closed, copy `LocalSplitScreen.dll` to `ue4ss\Mods\LocalSplitScreen\dlls\main.dll`, and
   `MultiMonitorSetup.exe` into `Mods\LocalSplitScreen\`.
4. Add `LocalSplitScreen : 1` to `Mods\mods.txt`, or put an empty `enabled.txt` in the mod's folder.

`python tools/game.py relaunch --deploy --label <name>` copies both files and starts the game. The build also makes a
`UE4SS.dll`, which is only there to link the mod against; the game keeps the release's.

The mod writes its settings to `LocalSplitScreen.ini` in its folder.

## Building

The build compiles UE4SS from its source, which must be the release the game runs. By default it is expected at
`deps/RE-UE4SS`; point `-DUE4SS_ROOT=<path>` at it if it lives elsewhere.

```
git clone --branch v4.0.0 https://github.com/UE4SS-RE/RE-UE4SS deps/RE-UE4SS
git -C deps/RE-UE4SS -c url."https://github.com/".insteadOf="git@github.com:" submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Game__Shipping__Win64
ctest --test-dir build -C Game__Shipping__Win64 --output-on-failure
```

This gives `LocalSplitScreen.dll` and `MultiMonitorSetup.exe` in `build/Game__Shipping__Win64/`, and the host tests.
The first build compiles UE4SS and takes a while; later ones only rebuild the mod.

The mod takes console commands on `127.0.0.1:4418`, from this computer only (`python tools/game.py shell`; `help` lists
the commands).

`python package.py` builds, runs the tests and zips the mod folder into `dist/`.

## Game version

The mod's addresses were read from one build of the game, whose `ff7remake_.exe` is named by a shortened SHA-256 at the
top of `src/engine_facts.h`. Before the mod patches the game's code, and before most of its calls into it, it checks
the bytes there against the ones it was written for; where they differ, that feature is left off and the log says why.

## Project map

```
src/
  engine_facts.h    every game address and offset the mod uses
  mod.cpp           the entry point
  modules           every module, in install order
  platform/         game-code checks and patches, safe memory access, the log, settings
  control/          the console on 127.0.0.1:4418
  players/          joining and leaving, the party member each player plays, pads, cameras, running one game call as
                    player 2 or 3
  battle/           command menus, targeting, battle cameras, enemy attention, battle options
  world/            NPCs, fades, field actions, talks, shops, catch-up warps
  views/            screen layouts, each view's camera fit, full-screen moments, multiple monitors
  hud/              each player's HUD
  render/           each player's picture: the split-screen render fixes, shadows, fog, the sun, movies
  audio/            sound heard from every player's character
  menu/             the Co-op rows in the main menu and on the title screen, and the Co-op page they open
setup/              the Multi Monitor Setup app
tests/              host tests, run without the game
tools/              console client and game control, virtual pad, commit checks, crash and performance tools
```

`tools/layout_doc.py` writes this map, and a test fails when it is out of date. Each module's header comment says what
it does. Keep the mod's headers `.h` and unlike UE4SS's names: `src/` is searched first, so a same-named header would
replace UE4SS's.
