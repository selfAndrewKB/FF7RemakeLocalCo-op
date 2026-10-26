# LocalSplitScreen (FF7 Remake)

Two-player split-screen for FINAL FANTASY VII REMAKE (Intergrade, UE 4.18), as a UE4SS C++ mod. Plan and research:
`../Documentation/plan/README.md` and `../Documentation/research/RE-*.md`.

## Keys

| Keys | What |
|------|------|
| Ctrl+F7 | Player 2 joins on the member in the 2nd slot of the party order: own End player controller, camera manager and field camera, the member taken through the game's leader funnel (with its own copy of the PlayerTrigger); it follows that slot. With the member not free (a script, an animation, a guide lead, a solo party) nothing is made yet: player 2 joins once it is free, and leaves again when a party change leaves it without one, so no second camera ever floats over an empty half |
| Ctrl+F8 | Player 2 leaves (also cancels a rejoin after a load, and a wait for a member) |
| Ctrl+F9 | Switch the battle menus' world slow-down: off, an open command menu no longer slows the world or greys the screen, so the fight and the ATB gauges run on at full speed; on again, the game's slow-down and grey (also `menu-slow on|off` and the settings file) |
| L3 twice (a further player's pad, in battle) | Bring that player's character into the fight when it is outside the fight's area (locked out behind a crawlspace, say): the game's own recovery warp, in front of player 1's character |
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
`bloom-origin on|off`, `distortion-apply on|off`, `pad-buttons on|off`, `menu-two on|off`, `menu-slow on|off` (off: an open battle command menu no longer slows the world or greys the screen), `menu-grey` (reading), `battle-warps players-stay|off|game` (who the game warps at a battle's start and end), `multi-monitor auto|on|off` and `column-order 123|132` (the settings file's options, switched in play; every option of `LocalSplitScreen.ini` is one row of the table in `src/platform/settings.h`, and its console command is built from that row), `targeting-two on|off`, `rates-two on|off`, `menu-two.leader`, `menu-two.route`, `window-span on|off`, `window-span.hold on|off` (the span with any player count, kept through loading screens), `mirror-player on|off` (an extra local player in every column no player fills shows player 1's picture, up to two; reading `mirror-player.state`), `view-columns 0..3` (the columns the window shows, side by side; 0 = as the span and the players decide; reading `column-layout.state`), `lock-camera-two.requests`, `lock-camera-two.restore`, `camera-override-two`, `enemy-leader`, `sequence-camera-two`, `target-cursor-two`, `party-events`, `log-level error|warning|info|debug|trace` (Debug by default; Trace holds the per-frame lines), `hud.dump roots|shown|all [<text>]`, `hud-band on|off`, `hud-image on|off`, `hud-targets on|off`, `hud-targets-log <renders>`, `movie-frame on|off` (a pre-rendered movie in every view; reading `movie-frame.state`); readings `p2.in`, `p2.member`, `p2.state` (out|awaiting|rejoining|in|playing|handed),
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

In the field each further player opens chests, picks up items, opens doors, uses the grappling hook and talks to NPCs
(quest NPCs and shopkeepers included) with their own pad and prompt. A talk plays as the game scripts it (on every
view, the whole game held; player 1's character, when more than 8 m away, is brought beside the player who started
it, facing the NPC). Each player walks only in the rooms (shops, homes) their own character stands in. A shop a further player opens zooms on that player's view and faces that player's
character; while any shop is up every pad drives it, as in the pause menu. Examine points and object events stay
player 1's.

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
and add `LocalSplitScreen : 1` to `Mods/mods.txt`. Close the game first. `LocalSplitScreen.ini` goes next to the
`dlls` folder (`tools/game.py relaunch --deploy` copies it when none is there): `[Views] multi-monitor = off | on |
auto`. `on` spans a row of equal monitors from the start and keeps the span whatever the player count (the title,
loading screens, the field and the menus on every monitor; every column no player fills mirrors player 1's picture
until a player joins); `auto` (the default) spans while player 2 is in; `off` leaves it to the console and Ctrl+F2.
`[Views] column-order = 1 2 3 | 1 3 2` picks which screen players 2 and 3 take. `[Battle] menu-slow = on | off`: off,
an open battle command menu no longer slows the world or greys the screen (Ctrl+F9 switches it in play). `[Battle] battle-warps =
players-stay | off | game`: who the game warps at a battle's start and end (players-stay, the default: every player's
own character stays where it is). The file is read once at the game's start (the log's `settings:` line lists every
option's word and where it came from; a word the file gets wrong is a warning and the default); a value's case and
spaces and commas do not matter. The install never replaces an existing `LocalSplitScreen.ini`: add a new section by hand from
the repository's copy; a key the file lacks takes its default.

## Layout

```
src/
  engine_facts.h            Every engine address and offset the mod relies on, as read in IDA (image base 0x140000000)
                            for ff7remake_.exe SHA-256 25B54C34...6635, with the research document that established it
  mod.cpp                   LocalSplitScreen for FINAL FANTASY VII REMAKE: the UE4SS entry point
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
    guarded                 Structured-exception guards for touching engine memory and calling engine code
    image                   The game's executable image in memory, and the translation from IDA addresses to live ones
    instruction_patch       Rewrites one instruction of the game's code in place, with one of the same length, and puts
                            it back
    log                     The mod's log: one line per event, written to UE4SS.log with a "[LocalSplitScreen]" prefix
    module_pin              Keeps this DLL loaded until the process exits
    seqlock.h               N floats one thread writes and others read (Boehm's seqlock): the writer bumps the version
                            to odd, stores the values, bumps it to even; a reader retries while it sees an odd version
                            or a version that changed under it
    settings                The mod's own settings file: LocalSplitScreen.ini in the mod's folder
                            (Mods/LocalSplitScreen/LocalSplitScreen.ini, one level above the dlls folder this module is
                            loaded from), read with the Windows profile API, so a user sets a …
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
    arrow_timers.h          Each seat's own timers for the off-screen enemy arrows, a pure store tested on the host
    bar_class.h             The enemy bar's class in a further column, a pure rule tested on the host
    field_icons             The field objective and item icons (the diamond with its line, FieldQuestIcon_C) placed
                            through each column's own camera
    hud_probe               Logging only: what the HUD is made of right now, for the HUD research and for checking each
                            HUD step live
    image_code.h            The machine code the further players' HUD images need at two spots that have no call to
                            redirect (RE-hud-two-images.md §3, RE-hud-overlay-pass.md §10)
    image_entries           The columns' layer entries on the render thread (RE-hud-two-images.md §3, §7: H2 and H3)
    layout_code.h           The machine code for the 16:9 assumptions in Square's HUD placement that misplace things in
                            a split half's band
    marker_replay           Battle world markers placed through each player's own camera
    minimap_rules.h         The navi map's pool-0 handover across the frame's HUD passes, as a pure rule
                            (hud/minimap_two.h holds the game side; the seats follow plan/seats.md step 3)
    minimap_two             Each seat's own minimap and compass: a navi map manager per seat, the game's own
    outside_icons           The off-screen enemy arrows (HUD slot 38: the red arrow at the view's edge pointing at an
                            enemy outside it) placed through each column's own camera
    overlay_band            Taken from Rebirth's hud/overlay_band unchanged in substance
    overlay_layout          Lays Square's HUD out inside the part of its UI image a split view shows, so each half shows
                            the whole HUD, anchored as a native screen of the half's shape, with its world markers on
                            their objects
    overlay_pass            The HUD passes by column (plan/seats.md step 4)
    pass_hide               Each view's HUD image shows only its own player's world cursors
    root_placement          Where the HUD's main roots sit in a split half: as in single player, each in its corner of
                            the half
    second_image            The further columns' HUD images: the game's own overlay rendered again each frame for each
                            further column (hud/overlay_pass.h), before the game's own render, into a layer entry per
                            column of the same pool-2 ring slot
    target_rotation.h       Which UI target each view samples, with N columns (hud/ui_targets.h; plan/seats.md step 5)
    ui_targets              One UI image per view, on the render thread (RE-hud-two-images.md §4, §5, §7: H1 and H4)
    user_widgets            The live UUserWidgets, kept current without walking every UObject
  players/                  the seats: the session, the party member and its funnels, the scopes, the pads, each seat's
                            camera and trigger
    action_key              One of the game's action getters (Input_IsActionHeld_ByKey, Input_IsActionPressed_ByKey) on
                            a copy of an action key, as the game calls them: the key is 24 bytes (FName, details object,
                            reference controller), passed by value, and …
    action_tables.h         A pad's own copy of the game's action state (RE-input.md §1.6-§1.7, RE-input-p2.md §2.3)
    camera_override_two     A seat's member's camera overrides reach that seat's camera (RE-lock-camera-ledger.md row
                            X3)
    field_exit              Player 2 leaves before the game leaves the field
                            (Remake/Documentation/research/RE-lifecycle-world.md §3, §8)
    field_hold.h            When a party member's field AI state leaves it a further player's to hold, and when a script
                            needs it back
    field_root              The field root and what the modules read off it, in one place: the game's field root object
                            (facts::kEndGameRootGlobal), its state manager (the field control lock's mask and counter
                            live there, as does the on/off table), the …
    game_state              The game's own state, read for the console and the log: Square Enix's EGameState byte on
                            AEndGameState (title, loading, play, pause, menu, leaving), its menu kind and pause flag,
                            and the field step machine on the field root (13 = …
    lead_gate               Whether a LEAD the level wrote on a further player's member is a guide (the member goes to
                            its AI, the player's view stays on it) or staging (the player keeps it), from the game's own
                            data
    local_player_list       The engine's list of local players, for the modules that add a local player of the mod's own
                            (players/session: player 2; views/mirror_player: the mirror in an empty column) through UE's
                            own paths, on the game thread
    menu_pads.h             Where a further pad's key events go while the game's menus need them
                            (RE-pause-menu-any-pad.md §2.2, §3.3)
    nav_area_two            A seat's nav-area component ticks in that seat's scopes (RE-operator-settings-two.md §2.5)
    operator_battle_mode    Each seat's field camera operator enters and leaves battle mode with player 1's
                            (RE-operator-settings-two.md)
    own_controller          Player 2's own PlayerController: the End camera manager's BeginPlay, made safe for a second
                            player
    pad_buttons             Each pad's buttons for its own player (RE-input.md §1, §8; RE-input-p2.md §2: the design, a
                            written exception)
    pad_presence            Which XInput users have a pad, asked of XInput itself once a second on the game thread (the
                            game's own input system polls the same API every frame; a disconnected slot's query is the
                            slow one, so never per frame)
    party_events            The game's party funnels, heard by the session (RE-slot-member.md §1.5, §2.5, §3; Rebirth's
                            player_two/party_events)
    party_member            Which deployed party member a further player plays, and when it is the player's to take
    party_seat              A further player's controller takes a party member through the game's own leader funnel, and
                            gives it back
    player_trigger          Each seat's own PlayerTrigger: a copy of the game's one placed trigger actor
                            (RE-player-trigger.md §4-§5, Fix A)
    player_two_camera       Each seat's own field camera: a further AEndFieldCameraOperator per seat, driven by the
                            game's own code
    scopes                  The game's single slots answered for one further player
    seat_objects            One registry of each seat's game objects: its player controller, its field camera operator
                            and the member it plays, kept as weak pairs (players/weak_object.h) so a destroyed or
                            replaced object resolves to null wherever it is …
    seat_state.h            A seat's state, derived from the session's record of it
    seats.h                 The seats: the further human players the mod adds (plan/seats.md)
    session                 The further players (seats) joining, taking their party member, and leaving, through UE's
                            and the game's own paths, on the game thread
    split_render            The single-player look in each half: the effects stock UE 4.18 switches off for a split
                            viewport
    unreal_call             A reflected engine call (UObject::ProcessEvent) under a structured-exception guard, shared
                            by the modules that drive the game's reflected setters (players/session, hud/root_placement)
    weak_object             UE4 weak object pointers as the game stores them: {the object's index in GUObjectArray, its
                            serial number}
  render/                   the picture per view: the ghost fixes, one family per player, shadows, fog, sun, the movie
                            frame, the haze watches
    ao_origin               Square Enix's AO downsample, stage 0, made to read each view's own rows (the split-screen
                            ghost)
    area_blend              Each player's view takes the indoor or outdoor look of its own camera (the user's report of
                            2026-10-25: player 1's Cloud walks into a building, the picture brightens for Cloud, and the
                            outside turns very bright in the other …
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
                            (render/family_now.h)
    family_boundary_list.h  The RHI command list and context layout the family boundary relies on
                            (render/family_boundary.h; RE-haze-occlusion.md Pass 10 I.1.4, I.1.5)
    family_now              Which player's family the renderer is drawing now, while one family per player is drawn
                            (render/one_family.h)
    family_split.h          One view family per player: the loop that hands a family of several views to the renderer
                            one view at a time
    fog_volume_watch        A watch on each view's fog volumes (RE-haze-occlusion.md, Pass 4): logging only, for the
                            blue haze in players 2's and 3's views
    history_cut             A test for the blue haze in players 2's and 3's views: the game's own camera cut on every
                            camera but player 1's, held for a number of frames
    index_ranges.h          Ranges of scene object indices as a test command writes them: "a b" pairs (from a, up to but
                            not including b), separated by commas, e.g
    lighting_volume         The translucency lighting volume with more than one view (RE-haze-per-view.md §2, §5, §9):
                            the blue haze in players 2's and 3's views
    movie_frame             The pre-rendered movie in every view (research/RE-movie-view.md, pass S-RMK-MOVIEVIEW,
                            reviewed in IDA 2026-10-22)
    occlusion_exempt        A test for the blue haze in player 2's and player 3's views: chosen objects skip their
                            occlusion test and count as visible, in all views or only some (the switch below)
    one_family              One view family per player: the game viewport's frame is handed to the renderer one player
                            at a time, each player the first and only view of its own family
    shadow_code.h           The machine code of the per-view shadow levers, built here as pure byte arrays and executed
                            over stand-ins on the host
    shadows_per_view        Shadows decided per view (RE-shadows-per-view.md, RE-shadows-per-view-build.md)
    ssr_origin              Square Enix's screen-space reflections made to read each view's own pixels (the column-3
                            ghost in player 2's view)
    sun_cascades            Each view's own sun cascades where the game takes every view's (RE-haze-per-view.md §3,
                            §9.10; rule: render/sun_owner.h)
    sun_owner.h             Whose sun cascade a view may use (RE-haze-per-view.md §3, §9.10)
    view_family.h           A render-thread view's family, read with the game's own fields (FSceneView::Family; the
                            family's Views array of view pointers and its count), the way the game's stereo rule reads
                            them (0x1425C4F10)
  world/                    the world around every player: fades, animation, NPCs, hiding, the field targets, walk-up
                            moves, talks, shops
    anim_choice.h           Which camera the game's animation judge reads for one mesh, and a further player's camera
                            snapshot in the game's own form
    anim_two_views          Characters near any player animate as the game animates them near player 1
    charapop_rule.h         CharaPop's Light/Middle rule, as its evaluators apply it to one camera, for the entries a
                            further player's camera needs (world/npc_detail.h)
    env_set_view.h          The pieces of world/env_sets_two that need no game: a further player's EnvironmentSet as the
                            mod keeps it (one per seat), paced the way the game paces player 1's, and the edit of a
                            player's hidden-actor list that removes only the …
    env_sets_two            Each further player's view hides what the area's design hides under that player, as player
                            1's view does under player 1
    field_targets           A further player opens chests, picks up the shining items on the ground, opens doors and
                            curtains and fires the grappling hook with their own pad, as player 1 does
    forced_walk             Every player walks where the story makes player 1 walk
    npc_crowd               The game's NPC crowd follows every player: each NPC is scored from the nearest player
    npc_detail              The NPCs a further player is near keep the detail the game would give them from that
                            player's own camera
    npc_senses              NPCs notice every further player as they notice player 1, player 1 first
                            (research/RE-npc-reactions.md; the user, 2026-10-21: "some NPCs that programmed to respond
                            to P1's presence as they walk by ...
    npc_senses_code.h       The pieces of world/npc_senses that need no game: the stimulus grid's cell rule, the choice
                            of its centre, and the machine code that puts that centre in place
    overlap_fade            No character turns see-through in a ladder, squeeze, crawl or similar move while a further
                            player is in
    party_fade              Party members never vanish near a camera while a further player is in
    search_arrows           Each player's own grappling-hook search arrow
    shop_act                A shop a further player opens is that player's: its camera zooms on the opener's view, the
                            shopkeeper and the opener's character face each other, and the opener's pad drives it
    talk_gather             Player 1's character is brought to a talk another player starts, when it is far
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
    column_layout           Three equal columns through the engine's own layout table (plan/seats.md step 5;
                            research/RE-three-views.md)
    column_rules.h          The pure rules of the view columns (plan/seats.md step 5: three columns)
    display_align           A monitor of the row lined up with the row's top through Windows' display-configuration API
                            (the one Display settings itself uses), for this Windows session only, and put back at the
                            leave
    letterbox_patch         Two views in the game's Borderless mode: Square Enix's single-view letterbox switched off
                            while they exist
    mirror_player           The mirror players: an extra local player of the mod's in every column no player fills,
                            showing player 1's picture, so a spanned window never shows one view across the whole row
    monitor_row             Which monitors a spanned game window may cover: a row of equal monitors
    scene_camera_two        The scene camera on player 2's view: a cutscene, a talk scene or an event camera plays on
                            player 2's view as it does on player 1's
    split_scale             Split views fill their part of the window whatever the game's resolution option
    split_scale_rules.h     The pure rule of views/split_scale: how far the split views must be scaled so they fit the
                            scene buffer
    window_span             The game window spanned across a row of monitors, one full 16:9 view per player, through the
                            game's own levers
  battle/                   battle per seat: the command menu, targeting, the cursor, the lock and sequence cameras, the
                            enemies' attention, the options
    arena_warp              A further player's character brought into the fight on a double click of the left stick (the
                            user, 2026-10-26: "it is sometimes possible to get locked outside of a fight accessible only
                            through a crawlspace, which is disabled in …
    battle_warps            Which party members the game warps at a battle's start and end
    coin_choice.h           Which human an enemy counts as "the player" for one decision
    coin_table.h            The coins enemies hold (battle/enemy_coin.h): a lock-free table keyed by the enemy's battle
                            AI controller, each coin the player it names (1..4) and when it was tossed, plus one mark
                            per coin: "an action has run since this toss" …
    command_camera_two      Each seat's Tactical Mode target camera: the field camera's Command flavor (13) reads the
                            seat's own menu
    command_menu_rules.h    The command menus' shared engine state, as rules without the game
    command_menu_tick       Each seat's command controller ticked at the game's own spot, inside the seat's scopes
    command_menu_two        Each seat's own battle command menu: a FieldCommandCtrl per seat, built and kept with the
                            game's own calls
    enemy_coin              Enemies share their attention between the humans: one coin per enemy decision
    enemy_leader            Enemies follow the human whose member stands when the game's leader is down
                            (RE-enemy-leader-reads.md §2, §5.1)
    enemy_watch             What each enemy is doing, for the live reads behind the circling report
    lock_camera_rules.h     The lock camera readings' arithmetic, without the game (battle/lock_camera_two.h applies it)
    lock_camera_two         Each seat's lock camera at the edges outside the operator's tick (RE-lock-camera-ledger.md;
                            the seats follow plan/seats.md step 3)
    member_rates            Each further player's member at the player's rates (RE-battle-member-two.md §2, L1;
                            Rebirth's player_two/player_hits)
    menu_grey               The open battle menu's grey follows the menu slow-down switch
    party_switch            Neither player switches onto a member the other human plays
    sequence_camera_two     The battle sequence cameras (abilities, enemy specials, summons) on each player's own camera
    target_cursor_two       Each seat's own battle target cursor: the game's cursor driver run again per seat
    targeting_two           Each seat's targeting: the game's own targeting tick run again, once per seat
    type_answer_code.h      The bytes of a small function that answers the game's inlined "is this member the player"
                            test for every human's member
  audio/                    sound for every player: a listener per further player, each sound measured from the nearest
    listener_rules.h        The listeners' decisions as pure functions (audio/listeners.h), tested on the host
                            (tests/listener_rules_test.cpp)
    listeners               Every player hears the sounds near them: each SEAD sound is measured from the nearest
                            player's listener
tests/                      40 host-side test programs (ctest), one per pure header or code stub; run without the game
```
