# LocalSplitScreen (FF7 Remake)

Two-player split-screen for FINAL FANTASY VII REMAKE (Intergrade, UE 4.18), as a UE4SS C++ mod. Plan and research:
`../Documentation/plan/README.md` and `../Documentation/research/RE-*.md`.

## Keys

| Keys | What |
|------|------|
| Ctrl+F7 | Player 2 joins on the member in the 2nd slot of the party order: own End player controller, camera manager and field camera, the member taken through the game's leader funnel (with its own copy of the PlayerTrigger); it follows that slot. With the member not free (a script, an animation, a guide lead, a solo party) nothing is made yet: player 2 joins once it is free, and leaves again when a party change leaves it without one, so no second camera ever floats over an empty half |
| Ctrl+F8 | Player 2 leaves (also cancels a rejoin after a load, and a wait for a member) |
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

Every module registers its own commands (`src/control/registry.h`), so `help` on the running build is the list.
Today: `join`, `leave`, `console <command>`, `report`, `quit`, `showflags <hex>`, `ao-origin on|off`,
`bloom-origin on|off`, `distortion-apply on|off`, `pad-buttons on|off`, `menu-two on|off`, `targeting-two on|off`, `rates-two on|off`, `menu-two.leader`, `menu-two.route`, `lock-camera-two.requests`, `lock-camera-two.restore`, `camera-override-two`, `enemy-leader`, `sequence-camera-two`, `target-cursor-two`, `party-events`, `log-level error|warning|info|debug|trace` (Debug by default; Trace holds the per-frame lines), `hud.dump roots|shown|all [<text>]`, `hud-band on|off`, `hud-image on|off`, `hud-targets on|off`, `hud-targets-log <renders>`; readings `p2.in`, `p2.member`, `p2.state` (out|awaiting|in|playing|handed),
`game.state` (title|loading|play|pause|menu|leaving|…), `game.paused`, `game.menu-kind`, `game.location`,
`field.step` (13 = steady field), `field.running`, `hud.overlay`, `hud.layout`, `pad.events`, `pads` (the XInput users with a pad), `lead.gate` (the last LEAD on player 2's member judged), `menu-two.state`, `targeting-two.runs`, `targeting-two.lock`, `tick`, `version`. The language's own verbs (`src/control/script.h`): `get <name>`, `status`, `help [<name>]`,
`wait <ticks>`, `wait-until <name> <op> <value> [<ticks>]`, `hold <player> stick <lx> <ly> <rx> <ry> <ticks>`,
`note <text>`, `abort`. A failure drops the commands queued behind it; queries answer even during a wait.

While player 2 is in, the mod also gives player 2's controller the game's input step right after player 1's (the game ticks only the primary controller's input), and forces back on the effects stock UE 4.18 turns off for split screen (show flags `0x100A`: TAA, bloom and bit `0x1000`). It makes Square Enix's AO downsample and first bloom reduce read each view's own rows; both read player 1's rows for player 2's view in the stock game (the split-screen ghost and its moving traces). The bloom fix works on D3D12 (the default renderer) and D3D11 (`-dx11`); where it cannot install, bloom stays off in split (`0x1008`).

While player 2 is in, the HUD is laid out for each half: the whole single-player HUD of a 32:9 screen in each half
(minimap top right, command menu bottom left), with world markers on their objects in player 1's half. Player 2's half has its own HUD image; player 1's talk and
examine cursors stay out of it. Its content is still player 1's otherwise (minimap, prompts): player 2's own is next.

While player 2 is in, each player's sticks and buttons are their own pad's (pad 1 = XInput user 0, also the
keyboard; pad 2 = user 1): pad 2's presses never open player 1's menus or release its held actions. In battle player
2 fights through its own command controller: its own command menu and party panel in its half, attacks, guard, dodge
and its Tactical Mode from pad 2, the world slowed by either menu, its own lock-on (R3) and target with the lock camera
on its own view, and its member fills ATB and staggers at the player's rates; neither player's switch lands on the other's
member. A virtual pad 2 for unattended tests: `python ../tools/pad2.py serve`, then `press A`, `hold RB --ms 2000`,
`stick L 0 1`.

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
  control/
    registry              every command a module registers: name, argument shape, set, get, help
    script                the language: a line against the registry, plus the runner's own verbs
    runner                the queue: order, waits, holds, failures, replies (host-tested)
    server                the TCP socket and its thread (127.0.0.1:4418)
    remote                the three joined on the game thread; log-level and tick
  hud/
    user_widgets          the live UUserWidgets, from a construction callback (no object walks; from Rebirth)
    hud_probe             logging only: hud.dump (every HUD widget's fields) and the hud.overlay reading
    overlay_band          the band arithmetic: visible band, layout size, host scale, clip map (host-tested; from Rebirth)
    overlay_layout        the HUD laid out in the band each half shows; markers on their objects (three call redirects)
    layout_code           the centred 1920x1080 frame and the off-screen arrows' edge in a band (two code redirects,
                          host-tested by executing them)
    root_placement        the minimap, command menu and party panel in their corners of the half (the game's setters)
    ui_targets            one UI target per view through the game's own pool (render thread; on with hud-image)
    second_image          player 2's HUD image: the overlay rendered first for player 2 (DeltaTime 0, fade kept,
                          one acquire, one submit; the render delegate's vtable slot and three call redirects)
    overlay_pass          which player's pass of the overlay render is running
    image_entries         render side: each entry drawn only in its own view, player 2's own vertex/index buffers
    image_code            the per-entry buffer load and the navi map's pool-0 skip (two code redirects, host-tested)
    pass_hide             player 1's field target icons and search arrows transparent in player 2's pass
  players/
    game_state            the game's own state, location and field step as readings, every change logged
    session               join and leave through UE's local-player path (player 2 exists only with a member: the join
                          waits outside the game for one); the member's hands to its AI with the seat kept; console commands
    field_exit            player 2 leaves inside the game's field-exit request (retry, load, jump, title);
                          session rejoins once the field is back (two call redirects)
    own_controller        the End camera manager's BeginPlay, made safe for a second player
    player_two_camera     player 2's field camera operator and the per-player scopes
    party_member          which deployed member a seat plays: the row after player 1's in the party order
    field_hold.h          the hold rule: which field AI states leave a member a further player's, which hand it to its AI
                          (animations, script layers) and which ask the game's data (LEAD) (pure)
    lead_gate             a LEAD on the member: a guide (to its AI, the player's view on it) or staging, from the
                          level's waiters on its arrival and the story's objective marker
    party_seat            a seat's controller takes its member through the game's leader funnel
    split_render          the single-player effects in each half (the engine's show-flag force mask)
  render/
    ao_origin             the AO downsample's view-rect origin (three call redirects)
    bloom_origin          the first bloom reduce's view-rect origin: a corrected copy of its pixel shader for
                          views not at (0,0) (four call redirects; D3D12 and D3D11)
    bloom_reduce_shader.h the corrected shader's DXBC (generated by Remake/tools/bloom_shader)
  world/
    party_fade            party members never vanish near a camera while player 2 is in (the game's own "don't
                          fade" reason list, as for the player's character)
    anim_two_views        each character's animation judged from the camera that sees it better (two vtable slots
                          of the judge, one of the budget allocator's Update)
    anim_choice.h         the camera choice and player 2's snapshot in the game's form (pure, host-tested)
    npc_detail            NPCs player 2's camera needs kept awake and shown (the game's per-NPC minimum level)
    charapop_rule.h       CharaPop's Light/Middle rule for one camera (pure, host-tested; from Rebirth)
    npc_crowd             each NPC scored from the nearer player, so the crowd spawns around both (one vtable slot)
    npc_senses            NPCs see player 2 as they see player 1, player 1's look first (the sensing grid's centre
                          moved toward player 2 when needed: one code redirect; the look's vtable slot)
    npc_senses_code.h     the grid's cell rule, the centre choice and its machine code (pure, host-tested by running it)
    env_sets_two          player 2's view hides what the area hides under player 2 (the game's area trace and read
                          run again for player 2; the engine's per-player hidden list); the world stays player 1's
    env_set_view.h        player 2's set kept and paced, the list edit (pure, host-tested)
  views/
    window_span           the game window across a row of equal monitors, one full view per player: the game's own
                          resolution request for the row, then the engine's window reshape; held, put back at the leave
    monitor_row.h         which monitors a spanned window may cover, and which to line up first (pure, host-tested;
                          from Rebirth, any row, tops within 32 px)
    display_align         a row monitor moved to the row's top for the Windows session (the display-configuration
                          API, nothing saved), put back at the leave
    scene_camera_two      a cutscene, talk or event camera on player 2's view too: player 1's manager's scene slots
                          set on player 2's manager with the game's own setters while the scene holds the players
                          (its blends seeded from player 2's own field camera, its cut and end reset repeated)
    letterbox_patch       the Borderless single-view letterbox off while two views exist (321 one-byte immediates)
  battle/ (the coin)
    enemy_coin            enemies share their attention between the humans: a coin per enemy decision at the game's
                          own decision gate, the script natives answered from it, both humans "the player" for the
                          hate rows, the enemy EQS keeping player 2's member, hits and sensing in the human's scopes
    coin_table.h          the coins, lock-free, keyed by the enemy's controller (pure, host-tested; from Rebirth)
    coin_choice.h         the toss: who may be "the player" and the fair pick (pure, host-tested)
    type_answer_code.h    the machine code answering the inlined leader-type test per member (pure, host-tested)
```
