# LocalSplitScreen (FF7 Remake)

Split-screen co-op for up to three players in FINAL FANTASY VII REMAKE (Intergrade, UE 4.18), as a UE4SS C++ mod. This page is
for working on the mod; the page for players is `PLAYERS.md`, which a release ships as its README. Plan and research:
`../Documentation/plan/README.md` and `../Documentation/research/RE-*.md`.

## Keys

| Keys | What |
|------|------|
| Start, on a pad that plays no one | Takes back a player whose pad is gone (a pad that died and came back on another number); otherwise a player joins on that pad (the Ctrl+F7 path below), their view on the next screen and their window open |
| View held a second | That player's own window, in their own view: d-pad up and down the character (players 2 and 3), d-pad left and right the player number (traded with the nearest player that way; each person keeps their pad and their screen and takes the other number's character, player 1's the party leader), LB and RB the view to another screen, A, Start, a B tap or View close it, B held a second leaves the game (player 1 cannot) |
| Ctrl+F7 | The keyboard's way in (Ctrl+Shift+F7 the same for player 3). Player 2 joins on the member in the 2nd slot of the party order: own End player controller, camera manager and field camera, the member taken through the game's leader funnel (with its own copy of the PlayerTrigger); it follows that slot. With the member not free (a script, an animation, a guide lead, a solo party) nothing is made yet: player 2 joins once it is free, and leaves again when a party change leaves it without one, so no second camera ever floats over an empty half |
| Ctrl+F2 | The window across the row of monitors: leaves it when in, else enters, and again on each join |
| Ctrl+F8 | Player 2 leaves; Ctrl+Shift+F8, player 3 (also cancels a rejoin after a load, and a wait for a member) |
| Ctrl+F9 | Switch the battle menus' world slow-down: off (the default), an open command menu no longer slows the world or greys the screen, so the fight and the ATB gauges run on at full speed; on, the game's slow-down and grey (also `menu-slow on|off` and the settings file) |
| L3 twice (a further player's pad) | In battle: bring that player's character into the fight when it is outside the fight's area (locked out behind a crawlspace, say): the game's own recovery warp, in front of player 1's character. In the field: bring that player's character back beside player 1's (left behind a door, across a drop, or just far away): the game's own catch-up move, with its fade-in; not while a scene, a talk or a field action holds either character |
| Ctrl+F6 | Log every local player's controller, pawn, camera manager and camera actor, and each player's input, movement and camera call counts |

## The console

The mod listens on `127.0.0.1:4418` (`src/control/remote.h`): one command per line, replies streamed back one line
each (`ok`, `failed`, `rejected`, `dropped`, `value`, `help`, `note`; `src/control/runner.h`). From the host:

```
python ../tools/game.py shell                                  an interactive console (tab completes commands)
python ../tools/game.py -c join -c "wait-until p2.state == playing" -c "hold 2 stick 0 1 0 0 120"
python ../tools/game.py run ../tools/scripts/smoke.txt         a file of commands, stopping at the first failure
python ../tools/game.py status | launch | quit | relaunch --deploy --label v0.8.0 | shot | keys Enter
```

Every module registers its own commands (`src/control/registry.h`), so `help` on the running build is the list, with
one line of help each. A switch is named after its module and takes `on|off` (all on at start unless noted); its
readings are `<switch>.<what>`, and an area's readings `<area>.<what>`. Today:

- **Players:** `join` and `leave` (player 2), `p3.join` and `p3.leave` (player 3); per seat `p2.state` and `p3.state`
  (out|awaiting|rejoining|in|playing|handed), `.member`, `.in`, `.rejoin`; `party.ai`, `party-events`, `pads` (the
  XInput users with a pad), `report`, `quit`, `console <command>`, `showflags <hex>`, `version`, `tick`,
  `log-level error|warning|info|debug|trace` (debug at start; trace holds the per-frame lines).
- **The settings file's options**, switched in play: `multi-monitor on|off`, `column-order 123|132`,
  `menu-slow on|off` (off: an open battle command menu no longer slows the world or greys the screen),
  `battle-warps on|off` (on: the game warps the whole party at a battle's start and end; off, the default: players 2
  and 3's characters stay where they are), the battle's rates
  `enemy-damage`, `enemy-hp`, `boss-hp`, `enemy-stagger`, `exp-gain`, `ap-gain` (each `0.1` to `10.0`). Every option of
  `LocalSplitScreen.ini` is one row of the table in `src/platform/settings.h`, and its command is built from that row.
- **The game:** `game.state` (title|loading|play|pause|menu|leaving|…), `game.paused`, `game.menu-kind`,
  `game.menu-owns-input`, `game.location`, `field.step` (13 = steady field), `field.running`, `field.targets`,
  `scene.playing`, `scene.skip`, `scene.state`, `camera-slots.one|two|three|mirror|mirror-2`.
- **Views:** `window-span`, `window-span.hold` (off at start: the span with any player count, kept through loading screens),
  `view-columns 0..3` (the columns the window shows; 0 = as the span and the players decide; an extra local player in
  every column no player fills shows player 1's picture), `single-view auto|on|off`, `scene-camera`,
  `split-scale`, `split-fov.horizontal`, `split-fov.vertical`, `shoulder-scale`, `target-lean-fix`, `letterbox-patch`; readings `narrow-camera.state`, `whole-screen`, `split-switch`, `column-layout.state`, `window-span.state`,
  `mirror-player.state`, `scene-camera.state`, `split-scale.state`, `split-fov.state`, `letterbox-patch.sites`.
- **The field:** `npc-crowd`, `npc-detail`, `npc-senses`, `env-sets`, `area-levels` (every player's area kept loaded,
  not only player 1's), `forced-walk`, `overlap-fade`, `search-arrows`, `anim`, each with its reading `world.<switch>`;
  `level-watch` (reading `world.levels`); `world.fade`, `lead.gate` (the last guide judgment: a LEAD or a run
  ahead on a further player's member), `lead.path <state>` (prints a state's chain), `nav-area`, `camera-override`,
  `shop-act`, `traversal.action`, `traversal.walkup`; `party-warp 2|3` (brings that player's character back beside
  player 1's, as their double click of L3 in the field does; reading `world.party-warp`); the two experimental talk options
  `talk-gather on|off` (player 1's character brought to a talk another player starts far from it) and `talk-follow on|off`
  (at player 1's own talk the other players' characters walk over on their AI, or stay standing with their players), with
  the reading `world.talk-gather`.
- **Pads:** `pad-buttons`, `pad-menus`; readings `pad.events`, `pad.to-game` (each further pad's counts),
  `pad.menu-opens`.
- **Battle:** `command-menu`, `command-camera`, `targeting`, `member-rates`, `enemy-coin`, `enemy-stall`, `member-area`; readings
  `command-menu.state`, `.leader`, `.gates`, `.route`, `command-camera.aims`, `targeting.runs`, `targeting.lock`,
  `target-cursor`, `lock-camera.jumps`, `.frames`, `.requests`, `.restore`, `sequence-camera`, `menu-grey`,
  `enemy-coin.counts`, `enemy-stall.counts`, `enemy-leader`, `enemy-watch`, `arena-warp`, `operator-battle-mode`.
- **The HUD:** `hud-band`, `hud-image`, `hud-catch-up`, `hud-minimap`, `hud-markers`, `hud-field-icons`,
  `hud-outside-icons`, `hud-targets` (off at start: the HUD image turns it on when the screen splits),
  `hud-targets-log <renders>`; readings `hud.layout`, `hud.overlay`, `hud.minimap`,
  `hud.markers`, `hud.field-icons`, `hud.outside-icons`, `hud.image-ms`; `hud.dump roots|shown|all [<text>]`.
- **Rendering and sound:** `ao-origin`, `bloom-origin`, `ssr-origin`, `distortion-apply`, `sun-cascades`, `area-blend`,
  `shadows-areas`, `shadows-sun`, `fog-areas`, `shadows-lamps`, `shadows-casters` (each view's shadow and fog
  distances and lamp shadows from its own camera),
  `one-family`, `movie-frame` (a pre-rendered movie in every view), `history-cut <n>`; readings `ssr-origin.shaders`,
  `sun-cascades.away`, `render.area-blend`, `movie-frame.state`, `history-cut.raised`, `shadows`, `fog-volumes`,
  `fog-lights`, `sound`; tests `volume-box` (off at start; reading `.views`), `occlusion-exempt <a b, c d>|none`, `occlusion-exempt.views`,
  `occlusion-exempt.third-column`.

The language's own verbs (`src/control/script.h`): `get <name>`, `status`, `help [<name>]`,
`wait <ticks>`, `wait-until <name> <op> <value> [<ticks>]`, `hold <player> stick <lx> <ly> <rx> <ry> <ticks>`,
`note <text>`, `abort`. A failure drops the commands queued behind it; queries answer even during a wait.

While a further player is in, the mod also gives each further player's controller the game's input step right after player 1's (the game ticks only the primary controller's input), and forces back on the effects stock UE 4.18 turns off for split screen (show flags `0x100A`: TAA, bloom and bit `0x1000`). It makes Square Enix's AO downsample and first bloom reduce read each view's own rows; both read player 1's rows for the further views in the stock game (the split-screen ghost and its moving traces). The bloom fix works on D3D12 (the default renderer) and D3D11 (`-dx11`); where it cannot install, bloom stays off in split (`0x1008`).

While a further player is in, the HUD is laid out for each view: the whole single-player HUD in each (minimap top
right, command menu bottom left), with world markers on their objects in every view. Each further view has its own HUD
image, with its own minimap, markers, arrows, target bar and battle menu; player 1's talk and examine cursors stay out
of it.

While a further player is in, each player's sticks and buttons are their own pad's (pad 1 = XInput user 0, also
the keyboard; pad 2 = user 1; pad 3 = user 2): a further pad's presses never open player 1's menus or release its held
actions. In battle each further player fights through its own command controller: its own command menu and party
panel in its view, attacks, guard, dodge and its Tactical Mode from its own pad, the world slowed by any menu, its own lock-on (R3) and target with the lock camera
on its own view, and its member fills ATB and staggers at the player's rates; neither player's switch lands on the other's
member. A virtual pad 2 for unattended tests: `python ../tools/pad2.py serve`, then `press A`, `hold RB --ms 2000`,
`stick L 0 1`.

In the field each further player opens chests, picks up items, opens doors, uses the grappling hook and talks to NPCs
(quest NPCs and shopkeepers included) with their own pad and prompt. A talk plays as the game scripts it (the whole game held; on every
view, except that in one window a scripted talk on the game's event camera shows on one view; player 1's character,
when more than 8 m away, is brought beside the player who started it, facing the NPC). Each player walks only in the rooms (shops, homes) their own character stands in. A shop a further player opens zooms on that player's view and faces that player's
character; while any shop is up every pad drives it, as in the pause menu. Examine points and object events stay
player 1's. While player 1 opens a chest, picks up an item or works a switch or a terminal, the further players keep
their characters and views; a story's guide (a lead, or a run ahead the level waits on) stays the player's by default,
its stop marked in that player's view and the character's AI finishing the last steps (Scripted Sequences on the Co-op
page; AI-controlled: the character's AI walks it with that player's view on it, and it comes back after).

In one window the views collapse to player 1's single view for menus, shops, cutscenes, minigames and scripted talks,
and split again after; on a monitor row the further columns show player 1's picture instead.

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

**Settings.** The mod writes `LocalSplitScreen.ini` next to the `dlls` folder itself, at every start, from its own
options table (`src/platform/settings.h`): each option with its Co-op page name, its help, its values and its default,
and the value read from the file before (a missing key takes its default). Change a value after its `=` and the next
start reads it; comments added by hand are not kept, and an option the mod no longer has is dropped (named in the
log). A choice on the Co-op page is saved into it at once; the console changes an option for the session only. The
log's `settings:` line lists every option's value and where it came from; a value the mod cannot read is a warning,
the option takes its default, and the value stays in the file as written, with a note. The repository ships no copy
of the file, so none can go stale.

**Choosing the monitors.** Copy `build/Game__Shipping__Win64/MultiMonitorSetup.exe` (Multi Monitor Setup) next to
`LocalSplitScreen.ini` and run it with the game closed: it shows the desktop as Windows arranges it, you click two or three monitors in the order you want
them, left to right, and **Save** keeps them with Multiple Monitors and Screen Order. Each monitor of the row is marked with the
player it shows. With none picked the game takes the longest row it finds. The game is then started from Steam as always; the
app need not stay open. Everything works by keyboard too: Tab and the arrow keys move, Left and Right change an option, Space
or Enter presses, Enter alone saves, Escape closes. Run from another folder, the app says the game will not read its file.

The monitors need not be next to each other in Windows. While the game holds the row they are moved into it, and afterwards they
go back: Windows' own saved arrangement is never changed, so a restart returns your layout whatever happens. If the game crashes
or is killed, the same exe, which the mod keeps running in the background as a guard, puts them back (its one line of result is
in `LocalSplitScreen.desktop.log`); if that fails too, the next start of the game or the app does, and the app shows a
**Restore Monitors** button whenever the desktop differs from Windows' saved arrangement. The exe must keep its name and its
place beside the settings file: the mod starts the guard from there.

A picked monitor that is not connected is not replaced by another. With two of three picks connected the row is made of those
two; with fewer than two the game stays in one window. The row grows or forms when a monitor arrives and shrinks or ends when
one leaves. A streaming host's virtual monitor (Apollo, Vibepollo, SudoVDA) works like any other and is recognised by its name,
so give each client a name of its own.

The app's look is drawn by its own code (`setup/view.cpp`, colours in `setup/theme.h`); its icon, `setup/app.ico`, is drawn by
`../tools/make_setup_icon.py`. Its file properties carry the mod's version, read by CMake from `src/mod.cpp`; its file name is the one the mod
starts the guard by, read by CMake and by `../tools/game.py` from `src/views/display_move.h` (`kSetupExe`).

Picked monitors may differ in size and Windows scaling: each view fits its own monitor, top-aligned, and the window's part below a
shorter monitor is simply not seen. A view wider than about 4096 px (a 32:9 monitor) shows a band of wrong colours at its right
side. With none picked, a row needs monitors of one height at 100 % scaling.

## Comments

A comment says what the code does and why, in plain sentences, and stays true as the code ages. It carries no date, no
version number, no quote of or credit to a person, and no history (`until`, `used to`, `now`): the reason behind a rule is
stated as a fact, and a fault the code prevents as what would happen without it. It does not name the Rebirth mod: where a
piece came from, and how the two mods differ, is in the topic's History in the documentation. Addresses live in
`src/engine_facts.h`, research in `../Documentation/research`: a comment names the game's function without its address (in a
machine-code stub, the file's named constant, with an offset where needed), and may name a research file, without a section
number or pass tag. It may mark a claim `[CONFIRMED]` or `[ASSUMPTION]`.
A hook enforces this (`../../.claude/hooks/comment_style.py`): an edit or a commit whose new comment text breaks the
rule is refused with the line and the reason; `python ../../.claude/hooks/comment_style.py check src tests` runs the
same check by hand, and a game term that matches by accident goes in `comment_style_allow.txt` beside it.

## Packaging

`python package.py` builds, runs the host tests and writes `dist/LocalSplitScreen-v<version>.zip`: the folder `LocalSplitScreen` to copy
into UE4SS's `Mods` folder, with `dlls/main.dll`, the setup app, `PLAYERS.md` as its `README.md`, the licence when the repository has
one, and an empty `enabled.txt`, which makes UE4SS start the mod without a line in `mods.txt`. `--no-build` zips what is built, and
refuses a DLL that is not this source's version. No settings file is packed: the mod writes it.

## Layout

```
src/
  engine_facts.h            Every engine address and offset the mod relies on, as read in IDA (image base 0x140000000)
                            for ff7remake_.exe SHA-256 25B54C34...6635, with the research document that established it
  mod.cpp                   LocalSplitScreen for FINAL FANTASY VII REMAKE: the UE4SS entry point
  modules.cpp               The module table: every module the entry point installs after the session, in install order,
                            each with its removal or none and the rows it needs (modules.cpp)
  platform/                 the image, verified functions and vtable slots, code and call redirects, guards, the log,
                            the settings file (shared design with Rebirth's)
    byte_scope              The game's single slots answered for one player for the length of a call
    call_redirect           Sends one `call rel32` (or one tail call, `jmp rel32`) in the game's code to a function of
                            ours instead of its usual target, and puts it back
    code_memory             The two memory operations the mod's code changes share (code_redirect.h, call_redirect.h):
    code_redirect           Sends execution at one verified spot in the game's code through a few bytes of our own, and
                            puts the original bytes back
    entry_redirect          Sends every call of one game function through a function of ours, which may call the game's
                            function itself (Original()), and puts it back
    game_array.h            An FString or TArray as the engine lays them out (Data, Num, Max), and a source FString over
                            the mod's own text for the game's string copies to read
    game_thread             The game thread: the one the engine ticks on
    game_vectors.h          The engine's FVector and FVector2D as this engine lays them out: 4-byte floats, packed, no
                            padding
    guarded                 Structured-exception guards for touching engine memory and calling engine code
    image                   The game's executable image in memory, and the translation from IDA addresses to live ones
                            and back (the IDA form of a live address, for log lines)
    instruction_patch       Rewrites one instruction of the game's code in place, with one of the same length, and puts
                            it back
    log                     The mod's log: one line per event, written to UE4SS.log with a "[LocalSplitScreen]" prefix
    module_pin              Keeps this DLL loaded until the process exits
    name_text               The text of an engine name (FName), for log lines and readings: the lookup goes through
                            UE4SS and is guarded, since a name read from memory the engine has let go can be anything
    seqlock.h               N floats one thread writes and others read (Boehm's seqlock): the writer bumps the version
                            to odd, stores the values, bumps it to even; a reader retries while it sees an odd version
                            or a version that changed under it
    settings                The mod's own settings file: LocalSplitScreen.ini in the mod's folder
                            (Mods/LocalSplitScreen/LocalSplitScreen.ini, one level above the dlls folder this module is
                            loaded from), read with the Windows profile API, so a user sets a …
    settings_write.h        The settings file (platform/settings.h) as the mod writes it: the whole text from the
                            options table (FileText), so every option, its words, its default and its help in the file
                            are the code's own and never a stale copy
    sparse_array.h          Walks the allocated elements of an Unreal TSparseArray (the element array of a TSet or TMap)
                            by reading memory only
    steady_ms.h             The steady clock in milliseconds, for the modules' timeouts and rate windows
    text.h                  Small text helpers with no home of their own
    verified_code           Finding engine functions and vtable slots only where the research says they are
    vtable_entry            Replaces one entry of an engine vtable with a function of our own, and puts it back
    x64_jump.h              Encoding of the one x64 instruction the mod writes into game code: `jmp rel32` (E9 + a
                            signed 32-bit displacement measured from the end of the 5-byte instruction)
  control/                  the console: the command registry, the language, the runner, the socket (127.0.0.1:4418)
    registry                The command registry: every switch, action and readable value the control console knows, as
                            the modules register them at install
    remote                  The console: the socket server, the runner and the registry joined on the game thread
    runner                  Runs the console's commands against the registry
    script                  The control language: one command per line, UTF-8, '#' starts a comment
    server                  The console's socket: a listening TCP socket on 127.0.0.1 and one thread that reads lines
                            from every client into a queue and writes replies out
  hud/                      the HUD per column: the band, the images, the passes, the markers, the minimap, the arrows
    arrow_timers.h          Each seat's own timers for the off-screen enemy arrows (hud/outside_icons; one store per
                            seat and no allocation), a pure store tested on the host
    bar_class.h             The enemy bar's class in a further column, a pure rule tested on the host
    field_icons             The field objective and item icons (the diamond with its line, FieldQuestIcon_C) placed
                            through each column's own camera: one canvas serves every view because the placement is one
                            stateless call per icon
    hud_probe               Logging only: what the HUD is made of right now, for the HUD research and for checking each
                            HUD step live
    image_code.h            The machine code the further players' HUD images need at two spots that have no call to
                            redirect (research/RE-hud-two-images.md, RE-hud-overlay-pass.md)
    image_entries           The columns' layer entries on the render thread (research/RE-hud-two-images.md)
    layout_code.h           The machine code for the 16:9 assumptions in Square's HUD placement that misplace things in
                            a split half's band (research/RE-hud-overlay-pass.md)
    lead_mark               A further player's own marker for the move it keeps: the game's own main-objective diamond,
                            placed in the world to guide that player
    marker_replay           Battle world markers placed through each player's own camera
                            (research/RE-hud-battle-markers.md), by capture and replay
    minimap                 Each seat's own minimap and compass: a navi map manager per seat, the game's own
    minimap_rules.h         The navi map's pool-0 handover across the frame's HUD passes, as a pure rule (hud/minimap.h
                            holds the game side)
    outside_icons           The off-screen enemy arrows (HUD slot 38: the red arrow at the view's edge pointing at an
                            enemy outside it) placed through each column's own camera
    overlay_band            The composite fits the UI image to a view by a centred cover fit
                            (research/RE-rdc-split-frame.md)
    overlay_layout          Lays Square's HUD out inside the part of its UI image a split view shows, so each half shows
                            the whole HUD, anchored as a native screen of the half's shape, with its world markers on
                            their objects
    overlay_pass            The HUD passes by column
    pass_hide               Each view's HUD image shows only its own player's world cursors: left alone, player 1's blue
                            talk and examine cursor shows in every other player's view
    root_placement          Where the HUD's main roots sit in a split half: as in single player, each in its corner of
                            the half: minimap top right, compass top centre, command menu bottom left, party panel
                            bottom right, and the two summon gauges (the …
    save_notice             A line of the mod's in the game's save-data notice, the box at the top right: the one notice
                            the game draws on the title screen
    second_image            The further columns' HUD images: the game's own overlay rendered again each frame for each
                            further column (hud/overlay_pass.h), before the game's own render, into a layer entry per
                            column of the same pool-2 ring slot
    target_rotation.h       Which UI target each view samples, with N columns (hud/ui_targets.h)
    ui_effect_pass          The UI particles (the summon gauge's glow and every other UI effect) placed and sized per
                            column
    ui_targets              One UI image per view, on the render thread (research/RE-hud-two-images.md)
    user_widgets            The live UUserWidgets, kept current without walking every UObject
    view_prompt             A few lines of text in one player's own view: the player windows where the game's own window
                            cannot be shown
    view_window             The game's own framed message window in one player's view: the player windows
                            (players/player_window; research/RE-game-widgets.md)
  players/                  the seats: the session, the party member and its funnels, the scopes, the pads, each seat's
                            camera and trigger
    action_key              One of the game's action getters (Input_IsActionHeld_ByKey, Input_IsActionPressed_ByKey) on
                            a copy of an action key, as the game calls them: the key is 24 bytes (FName, details object,
                            reference controller), passed by value, and …
    action_tables.h         A pad's own copy of the game's action state (research/RE-input.md, RE-input-p2.md)
    actor_fields.h          The fields of the game's actors that several modules read, one reader each: a character's
                            components, mesh, player slot and tags, an actor's root component, a component's owner, a
                            controller's camera manager and leader
    ai_move_guard           A further player's member is never given an AI-only move while its player holds it: handed
                            to its AI, an interest walk can loop on a ladder for minutes, and that loop must never
                            happen
    camera_override         A seat's member's camera overrides reach that seat's camera
                            (research/RE-lock-camera-ledger.md)
    dpad_keys               The d-pad's four keys as the engine names them (EKeys::Gamepad_DPad_*, facts::kKeyDPad*), in
                            one place
    field_exit              Every further player leaves before the game leaves the field
                            (research/RE-lifecycle-world.md)
    field_hold.h            When a party member's field AI state leaves it a further player's to hold, and when a script
                            needs it back
    field_root              The field root and what the modules read off it, in one place: the game's field root object
                            (facts::kEndGameRootGlobal), its state manager (the field control lock's mask and counter
                            live there, as do the on/off table and the …
    game_globals            The game's globals that more than one module reads, rebased once
    game_state              The game's own state, read for the console and the log: Square Enix's EGameState byte on
                            AEndGameState (title, loading, play, pause, menu, leaving), its menu kind and pause flag,
                            and the field step machine on the field root (13 = …
    lead_gate               Whether a LEAD or a run to a mark (MoveToTarget) the level wrote on a further player's
                            member is a guide (the member goes to its AI, the player's view stays on it) or staging (the
                            player keeps it), from the game's own data
    lead_keep_rules.h       Whether a further player keeps a member the level walks to its marks, and when the game
                            takes it over at a mark
    leash_rules.h           Whether a live AutoWalk act leashes player 1 to a further player's member
                            (players/lead_gate; research/ RE-lead-unlock.md)
    local_player_list       The engine's list of local players, for the modules that add a local player of the mod's own
                            (players/session: a further player; views/mirror_player: the mirror in an empty column)
                            through UE's own paths, on the game thread
    menu_pads.h             Where a further pad's key events go while the game's menus, minigames and bike chases need
                            them
    nav_area                A seat's nav-area component ticks in that seat's scopes
                            (research/RE-operator-settings-two.md)
    operator_battle_mode    Each seat's field camera operator enters and leaves battle mode with player 1's
                            (RE-operator-settings-two.md)
    own_controller          A further player's own PlayerController: the End camera manager's BeginPlay, made safe for a
                            joining player
    pad_buttons             Each pad's buttons for its own player (research/RE-input.md, RE-input-p2.md: the design, a
                            written exception)
    pad_presence            Which XInput users have a pad, asked of the game's own xinput1_3 once a second on the game
                            thread (the game's input system polls the same API every frame; a disconnected slot's query
                            is the slow one, so never per frame)
    pad_rules.h             The pure rules of each physical pad's input, as the pad table reads it (players/pad_table)
                            and the player windows hear it (players/player_window)
    pad_table               Which physical pad answers each of the game's pad slots (plan/topics/input.md;
                            research/RE-pad-slots.md)
    party_events            The game's party funnels, heard by the session (research/RE-slot-member.md)
    party_member            Which deployed party member a further player plays, and when it is the player's to take
                            (research/RE-slot-member.md)
    party_rank_rules.h      Which party member each of players 2 and 3 plays, as a place in the game's party order
                            (players/party_member; a player window's arrows, players/player_window)
    party_seat              A further player's controller takes a party member through the game's own leader funnel, and
                            gives it back
    player_trigger          Each seat's own PlayerTrigger: a copy of the game's one placed trigger actor
                            (research/RE-player-trigger.md)
    player_window           Each player's own window: joining, the character, where the view is, and leaving, by pad
    player_window_rules.h   The pure rules of the player windows (players/player_window)
    player_window_text.h    What a player's window says (players/player_window; the rules:
                            players/player_window_rules.h)
    scopes                  The game's single slots answered for one further player (platform/byte_scope.h holds the
                            mechanism)
    seat_camera             Each seat's own field camera: a further AEndFieldCameraOperator per seat, driven by the
                            game's own code
    seat_objects            One registry of each seat's game objects: its player controller, its field camera operator
                            and the member it plays, kept as weak pairs (players/weak_object.h) so a destroyed or
                            replaced object resolves to null wherever it is …
    seat_state.h            A seat's state, derived from the session's record of it (out, awaiting, rejoining, in,
                            playing, handed)
    seats.h                 The seats: the further human players the mod adds (plan/seats.md)
    session                 The further players (seats) joining, taking their party member, and leaving, through UE's
                            and the game's own paths, on the game thread
    single_view             The engine's single view: the whole screen player 1's view alone while further players are
                            in (their controllers, characters and pads untouched)
    split_render            The single-player look in each half: the effects stock UE 4.18 switches off for a split
                            viewport
    split_switch            The game viewport's stock split switch, UGameViewportClient::bDisableSplitScreenOverride:
                            while it is set, UpdateActiveSplitscreenType picks the None layout whatever the player
                            count, player 1's view fills the screen and every …
    state_path.h            A member's own chain of level states ahead of its current setting, walked as the game's data
                            lays it out
    unreal_call             A reflected engine call (UObject::ProcessEvent) under a structured-exception guard, shared
                            by the modules that drive the game's reflected setters (players/session, hud/root_placement)
    weak_object             UE4 weak object pointers as the game stores them: {the object's index in GUObjectArray, its
                            serial number}
  render/                   the picture per view: the ghost fixes, one family per player, shadows, fog, sun, the movie
                            frame, the haze watches
    ao_origin               Square Enix's AO downsample, stage 0, made to read each view's own rows (the split-screen
                            ghost)
    area_blend              Each player's view takes the indoor or outdoor look of its own camera
    area_chains.h           Each view's own chain of area light settings, a pure rule tested on the host over a model of
                            the game's setter
    bloom_origin            Square Enix's first bloom reduce made to read each view's own rows (the moving-content
                            traces in player 2's half)
    bloom_reduce_shader.h   Origin-aware copy of Square Enix's level-0 BloomReduce pixel shader (fix F1 of
                            RE-split-bit2-leak.md)
    distortion_apply        Square Enix's directional-blur distortion apply, made to draw each view's rectangle in its
                            own half (the streaks across the split line when a weapon trail crosses it)
    dxbc_fix.h              Edits to a shader's DXBC container for render/ssr_origin: one exact instruction swapped in
                            the shader program, and the container's checksum recomputed (D3D12 refuses a container whose
                            checksum is wrong)
    family_boundary         Between two players' view families, the game's own end-of-frame state reset, in RHI stream
                            order; and, before each family, which player's family the renderer draws next
    family_boundary_list.h  The RHI command list and context layout the family boundary relies on
                            (render/family_boundary.h; research/RE-haze-occlusion.md)
    family_now              Which player's family the renderer is drawing now, while one family per player is drawn
                            (render/one_family.h)
    family_split.h          One view family per player: the loop that hands a family of several views to the renderer
                            one view at a time (research/RE-haze-occlusion.md)
    fog_volume_watch        A watch on each view's fog volumes (research/RE-haze-occlusion.md): logging only, for the
                            blue haze in players 2's and 3's views
    history_cut             A test for the blue haze in players 2's and 3's views: the game's own camera cut on every
                            camera but player 1's, held for a number of frames
    index_ranges.h          Ranges of scene object indices as a test command writes them: "a b" pairs (from a, up to but
                            not including b), separated by commas, e.g
    lighting_volume         The translucency lighting volume with more than one view (research/RE-haze-per-view.md): the
                            blue haze in players 2's and 3's views
    low_health              The low-health red edge in each player's own view
    low_health_rules.h      The low-health red edge's rules for each player's view (render/low_health.h)
    motion_history          Every player's view keeps the motion of moving things: the renderer's table of where each
                            moving object was last frame steps once a frame, as with one player, not once per player's
                            family
    movie_frame             The pre-rendered movie in every view (research/RE-movie-view.md)
    occlusion_exempt        A test for the blue haze in player 2's and player 3's views: chosen objects skip their
                            occlusion test and count as visible, in all views or only some (the switch below)
    one_family              One view family per player: the game viewport's frame is handed to the renderer one player
                            at a time, each player the first and only view of its own family
    pass_marks              GPU events around each render pass of a view (PrePass, Shadow depths, Base pass, Lights,
                            Translucency, Post-processing, the Square Enix passes, ...), nested in the view's event
                            (render/view_marks.h), so a GPU trace splits each …
    shadow_code.h           The machine code of the per-view shadow levers, built here as pure byte arrays and executed
                            over stand-ins on the host
    shadows_per_view        Shadows decided per view (RE-shadows-per-view.md, RE-shadows-per-view-build.md)
    ssr_origin              Square Enix's screen-space reflections made to read each view's own pixels (the column-3
                            ghost in player 2's view)
    sun_cascades            Each view's own sun cascades where the game takes every view's
                            (research/RE-haze-per-view.md; rule: render/sun_owner.h)
    sun_owner.h             Whose sun cascade a view may use (research/RE-haze-per-view.md)
    view_family.h           A render-thread view's family, read with the game's own fields (FSceneView::Family; the
                            family's Views array of view pointers and its count), the way the game's stereo rule reads
                            them
    view_marks              GPU events around each player's view family, for GPU profilers (Nsight Graphics' GPU Trace,
                            PIX): "LSS view 1", "LSS view 2", ...
    view_places             Each view's place in the engine's list, told by the view's own state rather than where it
                            sits on screen
  world/                    the world around every player: fades, animation, NPCs, hiding, the field targets, walk-up
                            moves, talks, shops
    actor_place             Where an actor stands, read the one way: its root component's world location, guarded (null
                            actor, null root or a faulting read answer false, `out` untouched)
    anim_choice.h           Which camera the game's animation judge reads for one mesh, and a further player's camera
                            snapshot in the game's own form (research/RE-anim-two-views.md)
    anim_views              Characters near any player animate as the game animates them near player 1
                            (research/RE-anim-two-views.md)
    area_levels             Every player's area stays loaded and solid, not only player 1's: left to the game, the world
                            and much of its lighting unload around a further player, who cannot move until player 1
                            returns
    area_levels_rules.h     The pieces of world/area_levels that need no game: the one level request every player's area
                            shares (research/RE-world-streaming-levers.md)
    catch_up_move           The game's own way to put a character beside another on the field, as the modules that bring
                            a character over call it
    charapop_rule.h         CharaPop's Light/Middle rule, as its evaluators apply it to one camera, for the entries a
                            further player's camera needs (world/npc_detail.h)
    env_set_view.h          The pieces of world/env_sets that need no game: a further player's EnvironmentSet as the mod
                            keeps it (one per seat), paced the way the game paces player 1's, and the edit of a player's
                            hidden-actor list that removes only the …
    env_sets                Each further player's view hides what the area's design hides under that player, as player
                            1's view does under player 1
    field_targets           A further player opens chests, picks up the shining items on the ground, opens doors and
                            curtains and fires the grappling hook with their own pad, as player 1 does
    forced_walk             Every player walks where the story makes player 1 walk: left alone, in a forced walk player
                            1 and the AI walk but a further player runs
    level_watch             Logging only: what loads, shows, hides and unloads the world's levels, so the further
                            players' areas can be held by the game's own loader
    npc_crowd               The game's NPC crowd follows every player: each NPC is scored from the nearest player
                            (research/RE-npc-near-player-two.md)
    npc_detail              The NPCs a further player is near keep the detail the game would give them from that
                            player's own camera: the reach is the union of every further player's view
    npc_senses              NPCs notice every further player as they notice player 1, player 1 first: an NPC made to
                            respond to player 1 walking by responds to any player, and to player 1 when several are near
    npc_senses_code.h       The pieces of world/npc_senses that need no game: the stimulus grid's cell rule, the choice
                            of its centre, and the machine code that puts that centre in place
    overlap_fade            No character turns see-through in a ladder, squeeze, crawl or similar move while a further
                            player is in (research/RE-overlap-fade-two.md)
    party_fade              Party members never vanish near a camera while a further player is in: the game fades a
                            party member that comes too close to the camera, which suits one camera and not several
    party_warp              A further player's character brought back beside player 1's in the field, when that player
                            asks: a player can be left behind a door that shut, on the far side of a drop, or simply far
                            away
    party_warp_rules.h      A further player's character brought back beside player 1's in the field (world/party_warp.h
                            applies it), as pure rules: when the ask is refused, and where the character is put
    push_event_rules.h      Which push-button rows (a switch, a lever, a terminal; world/push_events.h) a player beyond
                            player 1 may work, and how the row is run for that player's character, as pure functions
                            over the row's names
    push_events             Players 2 and 3 work the game's levers and switches with their own pad, as player 1 does: a
                            push-button event (a hold while a gauge fills: the game's own event on the game's own data)
                            runs in that player's identity, on that …
    referenced_actor        The game's read through a character's referenced actor, guarded where that actor is not an
                            End actor (research/RE-referenced-actor-crash.md)
    run_catch_up            A companion a further player walked past its own story box is brought back beside player 1's
                            character when the level's run through that box goes to its AI: a character recorded as
                            having run past a box no member was meant to …
    search_arrows           Each player's own grappling-hook search arrow: left alone, player 1's search arrow hides
                            whenever another player's appears (research/RE-hookshot-arrows.md)
    shop_act                A shop a further player opens is that player's: its camera zooms on the opener's view, the
                            shopkeeper and the opener's character face each other, and the opener's pad drives it
    story_box_rules.h       The rules of world/story_boxes, without the game: which of a held member's story-box entries
                            wait, when a waiting one fires, and whether player 1's character has come to the box
    story_boxes             A further player's member never fires its own story box ahead of the story, and never loses
                            one: no line of dialogue is ever cut or lost (research/RE-held-member-story-boxes.md,
                            RE-tifa-barret-talk-order.md: in the Ch
    talk_gather             Player 1's character is brought to a talk another player starts, when it is far, since Cloud
                            has lines to speak in it (research/RE-talk-far.md)
    talk_gather_rules.h     Where player 1's character is brought when another player starts a talk (world/talk_gather.h
                            applies it), as a pure rule: the flat directions to try for a navmesh point beside the
                            presser, in order, and whom the moved character …
    talk_presses.h          A further player's NPC presses, waiting for the act the NPC's talk state queues
                            (world/shop_act, world/talk_gather), as a pure table: the rules without the game
                            (world/field_targets holds the game side: the press itself, the …
    target_icons            Each further player's own field target icons: the blue talk/examine/chest markers the target
                            pick shows, made, updated and swept by the game for that player and drawn in that player's
                            view only
    traversal_action        A further player's member runs a walk-up action (ladder, squeeze, crawl, monkey bars, climb,
                            jump, vault, slides, balance beam and the rest of world/traversal_walkup.h's list) as player
                            1's does: on its own pad, with its player's …
    traversal_walkup        Players 2 and 3 walk their member up to ladders, squeezes, crawls, monkey bars, climbs,
                            jumps, vaults, rubble, duck-unders, slides, fence gaps, steps, balance beams, fans, counter
                            gates, lifting ropes, gap jumps, pole slides and …
  views/                    the views: the span over a row of monitors, the columns, the mirror player, the scene
                            camera, the split scale
    column_layout           Three columns through the engine's own layout table; on a span of monitors of different
                            widths each column is its own monitor's width
    column_rules.h          The pure rules of the view columns
    column_source           Where each column's view comes from, decided in one place each tick, so that the scene
                            camera, the mirror player and the HUD's tick do not each decide a part with nothing naming
                            the outcome per column
    columns.h               The view columns: how many views the screen shows and whose each one is, read once per tick
                            from the engine's local-player list
    display_align           The monitors placed to make the row, for as long as the game holds it, and put back at the
                            leave
    display_config          Windows' display configuration as QueryDisplayConfig gives it: the outputs (paths) and their
                            modes, with the names that go with a path
    display_move            The desktop's monitor positions through Windows' display-configuration API, the one Display
                            settings itself uses, and the way back from a move by the record kept while they are moved
                            (views/row_journal for what it means, …
    game_window             The game's own window and resolution, reached through the engine: the levers the span
                            (views/window_span) pulls
    letterbox_patch         Two views in the game's Borderless mode: Square Enix's single-view letterbox switched off
                            while they exist
    mirror_player           The mirror players: an extra local player of the mod's in every column no player fills,
                            showing player 1's picture, so a spanned window never shows one view across the whole row
    monitor_list            The desktop's monitors as Windows has them now, for the mod's span (views/window_span) and
                            the setup app (setup/): each monitor's rect and effective DPI, its GDI device name, and an
                            id and a name from the display configuration
    monitor_picks           The monitors the player picked for the row, as the settings file keeps them, and the row
                            they give
    monitor_row             Which monitors a spanned game window covers: a row, one view per monitor
    narrow_camera           The camera in a view narrower than the game's image and spread across a window: two small
                            corrections beside the narrow view's FOV
    record_file             The record of moved monitors as a file beside the settings file, and the process that owns
                            it
    row_journal.h           The record of a desktop the mod has moved, on disk from just before the move until the
                            monitors are back, so whoever finds it can undo the move: the guard process when the game
                            dies (setup/guard.cpp), the next start of the game …
    scene_camera            The scene camera on every further player's view: a cutscene, a talk scene or an event camera
                            plays on each further player's view as it does on player 1's
    span_desktop            The desktop as the span (views/window_span) reads it: every monitor, which one holds the
                            game window, and the monitors the settings file names found among them; with the row those
                            give (views/monitor_picks) and the moves that …
    split_fov               A view wider than the game's 16:9 image (two players top and bottom, the top view of three)
                            keeps a fuller picture, and a full-height narrower one (two players side by side, the large
                            view of three down one side) the characters' …
    split_fov_rules.h       The split FOV rule as arithmetic, apart from the engine so it is tested on the host
                            (views/split_fov.h applies it)
    split_scale             Split views fill their part of the window whatever the game's resolution option, which can
                            be changed while the game runs
    split_scale_rules.h     The pure rule of views/split_scale: how far the split views must be scaled so they fit the
                            scene buffer
    whole_screen            Whether the game shows something that takes the whole screen, from the game's own state: the
                            reason the one-window single view collapses the split (players/single_view.h applies it;
                            views/column_source.h decides it as …
    whole_screen_rules.h    Whether the game shows something that takes the whole screen: the decision, pure, over the
                            signals views/whole_screen.cpp reads each tick (views/whole_screen.h names the game state
                            behind each one)
    window_span             The game window spanned across a row of monitors, one full 16:9 view per player, through the
                            game's own levers
  battle/                   battle per seat: the command menu, targeting, the cursor, the lock and sequence cameras, the
                            enemies' attention, the options
    arena_warp              A further player's character brought into the fight on a double click of the left stick: a
                            character can be locked outside a fight whose only way in is a crawlspace, which the game
                            closes while the fight lasts
    arena_warp_rules.h      The double click that brings a further player's character into the fight
                            (battle/arena_warp.h applies it on the Dash press): two presses within kDoubleClickMs are
                            one double click, and a pair once spent starts afresh
    battle_rates            The battle's rates: enemy damage, enemy HP, boss HP, the enemies' stagger gauge, EXP and AP,
                            each a multiplier from 0.1 to 10.0 set from the Co-op page, the console or the settings file
                            (platform/settings.h)
    battle_rates_rules.h    The battle's rates (battle/battle_rates.h): the multipliers' words and the arithmetic, done
                            the game's way
    battle_warps            Which party members the game warps at a battle's start and end (research/RE-battle-warp.md)
    battle_warps_rules.h    Who the game warps at a battle's start and end, as the setting decides it without the game
                            (battle/battle_warps.h applies it at the game's three calls)
    coin_choice.h           Which human an enemy counts as "the player" for one decision (battle/enemy_coin.h;
                            research/RE-enemy-attention-coin.md)
    coin_table.h            The coins enemies hold (battle/enemy_coin.h): a lock-free table keyed by the enemy's battle
                            AI controller, each coin the player it names (1..4) and when it was tossed, plus one mark
                            per coin: "an action has run since this toss" …
    command_camera          Each seat's Tactical Mode target camera: the field camera's Command flavor (13) reads the
                            seat's own menu
    command_menu            Each seat's own battle command menu: a FieldCommandCtrl per seat, built and kept with the
                            game's own calls (research/RE-command-menu-two.md)
    command_menu_rules.h    The command menus' shared engine state, as rules without the game
                            (battle/command_menu_tick.h applies them)
    command_menu_tick       Each seat's command controller ticked at the game's own spot, inside the seat's scopes
                            (research/RE-battle.md, RE-command-menu-two.md, RE-battle-member-two.md)
    encounter_watch         What the field encounter sees, for the option "enemies notice players 2 and 3"
                            (research/RE-field-battle-start-seats.md)
    enemy_coin              Enemies share their attention between the humans: one coin per enemy decision
                            (research/RE-enemy-attention-coin.md)
    enemy_leader            Enemies follow the human whose member stands when the game's leader is down
                            (research/RE-enemy-leader-reads.md)
    enemy_stall             An enemy aimed at a human it cannot reach picks again: a MonoDrive at full ATB, aimed at a
                            player 28 m away, was seen to wait 95 s with another player 0.7 m from it
    enemy_watch             What each enemy is doing, for the live reads behind the circling report
    lock_camera             Each seat's lock camera at the edges outside the operator's tick
                            (research/RE-lock-camera-ledger.md)
    lock_camera_rules.h     The lock camera readings' arithmetic, without the game (battle/lock_camera.h applies it)
    member_rates            Each further player's member at the player's rates (research/RE-battle-member-two.md)
    member_state.h          A party member's battle state as the game keeps it on its battle player component, read
                            guarded (any thread)
    menu_grey               The open battle menu's grey and sound muffle follow the menu slow-down switch: with the slow
                            off, a grey screen and a muffled mix under a world that runs on would be all that is left of
                            the wait
    menu_grey_rules.h       The open battle menu's grey under the slow-down switch (battle/menu_grey.h applies it), as
                            pure rules over what the game's UI filter 0 shows: at the wait's spawn, at a menu-close
                            restore, at a flip of the switch and at the tick …
    party_switch            Neither player switches onto a member the other human plays (research/RE-battle-switch.md)
    party_switch_rules.h    The battle switch's rules in co-op (battle/party_switch.h applies the first,
                            players/session.h the others), as pure functions over what the game reports; tested on the
                            host
    seat_toad               Player 2's or 3's character turned into a toad, and back, with that player's own controller
                            and camera on it (research/RE-toad-seat.md)
    sequence_camera         The battle sequence cameras (abilities, enemy specials, summons) on each player's own camera
                            (research/RE-seq-camera-two.md)
    stall_rules.h           When an enemy that is ready to attack but declines counts as stalled, and whether the game
                            may pick again for it
    target_cursor           Each seat's own battle target cursor: the game's cursor driver run again per seat
                            (research/RE-target-cursor-two.md)
    targeting               Each seat's targeting: the game's own targeting tick run again, once per seat
                            (research/RE-battle-member-two.md, RE-battle.md)
    type_answer_code.h      The bytes of a small function that answers the game's inlined "is this member the player"
                            test for every human's member
  audio/                    sound for every player: a listener per further player, each sound measured from the nearest
    listener_rules.h        The listeners' decisions as pure functions (audio/listeners.h), tested on the host
                            (tests/listener_rules_test.cpp)
    listeners               Every player hears the sounds near them: each SEAD sound is measured from the nearest
                            player's listener (research/RE-audio-listener.md, plan/topics/audio.md)
  menu/                     the mod's own menu: a Co-op row in the game's main menu opening a page of the game's Options
                            screen
    coop_menu               The Co-op row of the game's main menu and the page it opens
    coop_menu_rules.h       The Co-op page's rules: what the game's Options screen shows when the mod opens its own copy
                            from the Co-op row of the main menu
    coop_page               The Co-op page itself: the mod's own instance of the game's Options screen, filled with the
                            settings table's options, and its value reads and commits
    coop_page_open          Opening and closing the Co-op page over the main menu, as the game shows and closes its own
                            windows
    coop_slider             The Co-op page's sliders: a list of a hundred values runs past the screen's bottom, so an
                            option with that many is a slider of the game's own, made to scroll
    game_ui                 The game's UI pieces the Co-op menu's modules share: the main menu's first page, weak
                            pointers held in the game's widgets, and FStrings read and copied the game's way (the UI
                            system's managers are players/game_globals' UiSlot)
    title_row               The Co-op row of the title menu, directly below Options, so a player sees it on starting the
                            game
    title_row_rules.h       Where the title menu's Co-op row goes among its rows' kinds (research/RE-title-menu.md):
                            directly below Options, so the rows through Options keep their places
tests/                      69 host-side test programs (ctest), one per pure header or code stub; run without the game
```
