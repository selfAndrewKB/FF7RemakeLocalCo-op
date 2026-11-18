# LocalSplitScreen

Local split-screen co-op for up to three players in FINAL FANTASY VII REMAKE INTERGRADE, as a UE4SS C++ mod.

## Requirements

- FINAL FANTASY VII REMAKE INTERGRADE on Steam.
- UE4SS 4.0.0 (`UE4SS_v4.0.0.zip` from the RE-UE4SS releases on GitHub). The mod is built against that release's
  headers and runs on no other UE4SS (an older one fails to load it with error 0x7f).
- To build: Visual Studio 2022 with the C++ desktop workload, CMake 3.22 or later, a Rust toolchain (UE4SS builds part
  of itself with it), Python 3.11 or later (a test and the tools), and a GitHub account linked to Epic Games (UE4SS
  fetches Unreal's headers from Epic's repository).

## Installing

1. Unpack UE4SS into `FINAL FANTASY VII REBIRTH\End\Binaries\Win64\`: placing the `dwmapi.dll` file and the `ue4ss` folder beside `ff7remake_.exe`
2. Unpack the mod's zip into the same folder. One file it contains is `UE4SS-settings.ini`, which has `HookLoadMap = 0`: with it on, every exit crashes.
3. Optional, to play across multiple monitors: put `MultiMonitorSetup.exe` in `ue4ss\Mods\LocalSplitScreen\`.

The mod writes its settings to `LocalSplitScreen.ini` in that folder.

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
The first build takes a while; later ones only rebuild the mod.

`python tools/game.py relaunch --deploy --label <name>` installs a build: `LocalSplitScreen.dll` as
`ue4ss\Mods\LocalSplitScreen\dlls\main.dll` and `MultiMonitorSetup.exe` in that folder. It turns the console on and
starts the game.

With `console = on` under `[Development]` in its settings file, the mod takes console commands on `127.0.0.1:4418`, from
this computer only (`python tools/game.py shell`; `help` lists the commands). It is off by default.

`python package.py` builds, runs the tests and zips the mod with UE4SS's game config for this game into `dist/`;
`--nexus` leaves the setup app out.

## Game version

The mod is written for the Steam version of the game whose `ff7remake_.exe` has file version 1.0.0.7 (SHA-256
`25B54C345C94EE9F6C6876B9C7537A69E6B0EEF25F322207C32A8DDCEE816635`, also named at the top of `src/engine_facts.h`).
Before the mod patches the game's code, and before most of its calls into it, it checks the bytes there against the ones
it was written for. Where they differ, as on another version, that feature stays off and the log says why.

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
tools/              console client and game control, virtual pad, commit checks, releases, crash and performance tools
```

`tools/layout_doc.py` generates this map from the folders on disk and the descriptions written in the script. A host test fails when the map is out of date. After adding or moving a folder, add its description to the script and run `python tools/layout_doc.py`.

For what a module does, read the comment at the top of its header.

Name the mod’s headers `.h` (UE4SS’s use `.hpp`), and don’t reuse a UE4SS header’s name. The build looks in `src/` before UE4SS’s folders, so a mod header with the same name would be used in place of UE4SS’s. The exception is `src/ue4ss_compat/`, which replaces a few UE4SS headers on purpose.
