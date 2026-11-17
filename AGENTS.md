# AGENTS.md

How changes to this mod are made, by people and by the coding agents they bring. `README.md` covers building and
installing, `CONTRIBUTING.md` the licence and sending changes. Where an instruction from the
maintainer and a rule here disagree, follow the maintainer and say which rule you set aside.

## Every change

1. **Understand the game's system first, statically.** Who writes the value, who reads it, when, on which thread, in
   which phases. A disassembler (IDA, Ghidra) and the game's reflection data answer most of this before the game is
   ever run.
2. **Map the edges before writing.** List every reader of the value you will change, every phase of the sequence it
   belongs to, every player count and screen layout it must hold for. Write the list down. A missed reader found in play
   costs a whole round trip; found on paper it costs a minute.
3. **Decide the co-op result.** Usually: each player's view looks and behaves like single player.
4. **Build it through the game's own levers, with its log lines inside.** The game's setters, slots and handlers, not a
   copy of them. The log lines are written with the code, not after the first bug.
5. **Have it reviewed** by a separate reviewing agent, given the diff and this file. Fix what it finds, and send it
   back until it approves.
6. **See it work in the game,** and confirm it from its own log lines, with a test that would have come out
   differently if it were broken.
7. **Record it:** the documentation in the same piece of work, a hand-made commit with a plain title, a version bump and
   a tag for a milestone.

## The rules that matter most

1. **Drive the game's own systems.** First decide the co-op result, then reach it through the levers the game itself
   uses: its setters, slots, input handlers, layout rules, its own refresh and synchronise calls.
   - Recreating, re-implementing or bypassing game behaviour is the rare exception: polling input the game already
     delivers, writing state the game writes itself, a parallel driver beside the game's.
   - It is allowed only when no game path exists, and the reason is written down (research notes and the plan) before
     it ships.
   - Exception: frame-rate work may change anything, but only after the disassembly shows the setting is read on the
     live path and a measurement shows it helps. Anything a player can see changing needs screenshots and the
     maintainer's agreement.
   - *Example:* to show a help line on two lines at a tighter spacing, the mod writes the help bar's own line-height
     property and calls the bar's own `SynchronizeProperties`, the path the game uses for every property. It does not
     draw its own text over the bar.
2. **Single player is the reference.** Compare with single player before changing anything a player sees.
   - Match the cause, not just the outcome. Something that happens on a button press in single player must happen on
     that player's press, not because the mod did it by itself. Before calling a gap "as in single player", name the
     game's trigger for it.
   - Keep the game's animations, fades and slides; never skip them unless the animated path provably cannot run.
   - Never cut a line of dialogue.
   - Cameras only adapt to split screen. No reframing, no centring, no change of the game's framing.
3. **Never trust an address or offset you have not verified.** Every engine address and struct offset is a named fact
   in `src/engine_facts.h`, found and byte-checked at install (code bytes a module checks live in a `*_code.h` beside
   it).
   - A raw offset in a module (`node + 0x18`) is a defect, even when it is right: name it in the facts header.
   - Never write to an offset worked out only on paper: read it live first.
   - A global's address says nothing about whether it holds the object or a pointer to it. Look at how the game reads
     it (`mov reg, [rip+g]` loads a pointer, `lea reg, [rip+g]` takes the object's address) and at any module that
     already uses it, and write which it is in the fact's comment. Getting this wrong once hung the game.
   - Addresses belong to one game build. Never carry one over from another build or from the sibling Rebirth mod.
4. **Fail safe, whole or none.** A feature whose checks fail logs why and stands aside. It never crashes the game and
   never half-installs.
   - Place a group of redirects that only work together all at once, or not at all; undo the first if the second fails.
   - Read and write game memory through the guarded helpers (`platform/guarded.h`). A stale pointer must fault safely.
   - A call that fails after a write puts the old value back.
5. **The log is how you see the game.** Nothing is built without its log lines. The method is the next section; it is
   as important as rule 1.
6. **Done means seen working in the game.** Before calling something done:
   - run it in the game (the keys, the console, a screenshot, the log);
   - quote the log lines that show it worked, with their timestamps;
   - use a test that would fail if the feature were broken: its own on/off switch, per-view state lines, a value made
     different on purpose for one player and read back for both.

   If you could not check it live, say "unverified", plainly. "It installed" is not "it works".
7. **Three players, every combination.** Code is indexed by seat, never written for "player 2" alone. Any change must
   hold for two and three players, one window and a row of monitors, and each screen layout. Build and play two players
   first; never write a path that only two can take.

## Seeing the game: the logging method

Whoever develops this mod is not always watching the screen, and a coding agent cannot watch in real time: it reads files. Someone or something plays; afterwards the log is the only witness. Almost every bug in this project was found, and almost every fix confirmed, by reading the log. That only works because of how the lines are written.

**What a line is.**

1. **Levels, switched while running.** `platform::Log` (Info: state changes, a few a minute at most), `LogDebug` (one
   line per event), `LogTrace` (per frame or per input), plus `LogWarning` and `LogError`; `platform::Enabled(level)`
   before building an expensive argument. The console command `log-level` changes the level live. The mod starts at
   Debug. A disabled line costs one comparison.
2. **The game's own names, not addresses.** Level names, class names, act and trigger names, state names next to their
   numbers, input action names: `level watch: ... CharaSpec_SU0000_00_Ifrit_Standard ... loaded yes`,
   `pad 2's press of Gamepad_FaceButton_Bottom set ... CommandMenu`. An address appears only beside a name, to tell two
   of a kind apart. The words around the names are plain ("player 2's character"), because the person reads the log
   too.
3. **Every decision says what it decided and why,** the refusals and the "no change" included:
   `left to the game: not a hate enemy (a boss, summon or mine: its script picks)`,
   `player 2's join refused: <the game's own wait reason>`. A silent "no" is the commonest invisible bug.
4. **Both ends of every state,** each with its cause: `whole screen: a summon's introduction` ...
   `whole screen: none`; a window that `opens (pad 2 held View)` ... `closes (the player is out)`. Number an event that
   spans several lines, so all of its lines line up.
5. **On change, not every tick.** A value read every frame is logged when it changes.
6. **Summaries for what is per frame:** one line per window of time with averages and worst cases (the frame watch
   writes one every 10 s).
7. **The start-up says what installed.** The version line, each module's "installed" or the reason it stood aside, the
   settings line with each value's source. Read it after every install.
8. **Watches that only report** (`level_watch`, `enemy_watch`, `encounter_watch`, `frame_watch`, `fog_volume_watch`,
   `hud_probe` and others) stay, even when their first question is answered: the question reopens with the next game
   update or field report. A watch never owns a decision.
9. **Live readings:** the console (`127.0.0.1:4418`) answers `get <reading>` for about two hundred named readings of the
   running game; `help` lists them.
10. **Logs are kept:** the previous log is kept at each launch, and a reported play session is saved with its version
    and date.

**When lines are written.** Log first, then build, for behaviour that is unknown and easy to reach. Log inside the fix,
for paths that are rare or tied to story moments (design from the research, ship the fix with its lines, let normal
play report each path). Log alongside every new mechanism. Never after the first bug.

**Volume is a level problem.** A log that is too loud gets its loud lines moved to a lower level or into a summary.
A diagnostic is deleted only with the code it watches, never to quieten the log; and never add a rate limiter or a
per-session cap: a cap once silenced the failure lines it shared a counter with.

**Reading it.**
- After every install, read the start-up for refusals before anyone tests.
- Match a person's report to the log by its time, and read forward and back from there. Their report is evidence, not
  something to be reproduced.
- Confirm a fix by its own lines, quoted with timestamps; without them it is "confirmed by eye only".
- Trust an absence only when the line's level was on and its path is known to log.
- Make every line true in every branch. A line that says `(player 1's answer)` when player 1's controller was never
  asked is a lie the next reader will believe.

## Investigating the game

- **Static first.** The disassembly, the reflection dumps (UE4SS can dump every reflected class with its member
  offsets) and the game's assets answer most questions before a single launch.
- **Never guess a cause.** Name it from evidence (the log, a decompile, a measurement) or call it unknown. Every claim in
  research notes is marked `[CONFIRMED]` (a decompile, a live test, or two sources), `[ASSUMPTION]` or `[UNKNOWN]`, and
  nothing is built on an assumption as if it were confirmed.
- **Port the whole system.** When the mod touches a game system for players 2 and 3, list every place the system reads
  "the player" or "the leader" and every value it shares, mark each one as handled or not, and build every gap in one
  pass. The goal is an empty "not handled" column, not the one symptom explained. A system ported one reported bug at a
  time produces a report a day.
- **Trace a signal through the whole sequence.** If a behaviour keys on a game state, show that the state is held in
  every phase of the sequence (entry, play, retries, results, hand-back), with the places that set it and clear it. A
  flag seen at a sequence's start may only flash.
- **The person's observation is evidence.** When someone who watched the game states a correlation ("it only appears
  when player 1 is near and looking at it"), take it as the finding and research its mechanism at once. Logs are for
  what they could not see.
- **Ask about the world before building logging to find it out.** "Is there a chest near this save?" is one question;
  three debug builds to find out could be an entire afternoon.
- **Answer what you can answer now.** Before listing something as unknown or as someone else's chore, check whether the
  assets, the disassembly, the logs or your own tools answer it, and do it.
- **Name and comment in the disassembler as you go**, persistently, so the next session does not repeat the work.
  Comments there record what was read, never what is proposed.
- **A tool that looks wrong is stop-and-report.** An impossible image base, decompiles whose globals look shifted, a
  database that will not reopen: stop, report it, find the cause. Never footnote it and work around it.
- **Do address and number-base arithmetic in a tool, never in your head:** Python, the disassembler's scripting
  console, the project's crash tools. One slipped digit once turned `0x1488EB8F0` into `0x1408EB8F0`; the wrong
  function was studied and a confident, wholly wrong conclusion followed. A conclusion that rests on one translated
  address stays an assumption until a second route (a name, a string, a call from a known function) agrees.
- **Write the research down** with the mechanism, how it was found (so the next game can find it again), what is stock
  Unreal Engine and what is Square Enix's `End*` code, and which addresses belong to which build.

## Players, seats and views

- Player 1 is the game's own player. Each player the mod adds is a **seat** (1 and 2 for players 2 and 3), with its own
  controller, camera, party member, command menu and HUD.
- The game keeps one copy of much that is really per player: the primary controller, the camera operator, the party
  leader, the command controller slot, the input focus. A **scope** (`players/scopes.h`) puts a seat's values into
  those slots around one game call and puts the game's back afterwards. Inside a seat's scope, the slot holds the seat's
  value: when you need player 1's while one may be open, use the helper that records it
  (`scopes::PlayerOneCommandController`), never the live slot.
- A **column** is one picture on screen, whoever owns it; a mirror view has a column but no seat.
- The world ticks once. A second view must never cause a second tick of the world, the scripts or the damage.

## Code

- **One helper, one predicate, not copies.** Before writing a reader, a walk, a condition or a utility, search for one
  (`players/`, `platform/`, `menu/game_ui.h`, the module's own file). A game global is rebased in one place, a pointer
  hop through an offset has one accessor, an engine type has one mirror (`platform/game_vectors.h`,
  `platform/game_array.h`). The same condition written twice is a second point of failure.
- **A feature in four parts.** Its decisions as pure rules in a header that touches no engine and is tested on the host
  (`battle/battle_warps_rules.h`, `views/whole_screen_rules.h`); one thin adapter at the game lever it drives; drawing
  that only draws what it is told; one place that ties them together each tick.
- **Delete what has served.** A one-shot probe comes out in the change that uses its answer. Dead code is deleted, not
  kept "just in case". A development watch is not a one-shot probe: it stays.
- **Nothing kept by hand that restates the code.** Generate it or drop it. The settings file is written by the mod from
  `src/platform/settings.h`; the README's project map is written by `tools/layout_doc.py`, and a test says when
  it is out of date.
- **New features ship on by default,** with their logging. Options go in the settings table, never in ad-hoc toggles.
- **Comments say what the code does and why,** in plain sentences that stay true as the code ages. A comment has no
  dates, version numbers or history ("used to", "now", "until"), no quotes of or credit to people, no mention of the
  sibling mod, no raw addresses (name the game's function; the address is a fact; in a machine-code stub, the file's
  named constant, with an offset where needed), no research section numbers or pass tags, no pointers into the plan.
  It may name a research file and mark a claim `[CONFIRMED]` or `[ASSUMPTION]`.
- **Never edit comments or names with a regex or a script sweep.** Read each site and edit it by hand; a pattern cannot
  tell which uses are meant.
- **Look up a real API's signature before calling it.** Never write against an API you guessed.
- **Costs that matter here:** a UE4SS object search (`FindFirstOf`, `FindAllOf`) takes about 20-25 ms in this game, so
  never per frame: cache the result and check it is still alive. Work on the game thread unless the code says
  otherwise; an unload may run off it, and then no call into the game's UI is allowed.
- Sources are CRLF. Keep each file's existing line endings.

## Names and words

- In code, a player the mod adds is a seat: `seat` + the thing (`seatController`, `seatMember`, `AnySeatMember`),
  `...One` or `playerOne` for player 1's (`operatorOne`), `column` for what exists once per picture.
- Never use "further", "second" or "two" in a new name for an added player.
- Text a player reads (log lines, console help, the README, the Co-op page) uses plain words: "player 2", "players 2
  and 3", "another player". Never "seat", never "further".
- In plans, reports and commit titles, describe what happens to the player and their character, not the project's
  internal terms.
- The sibling mod for FINAL FANTASY VII REBIRTH solved many of the same problems. Before new ground, read what it built
  and proved on the subject; take the proven design and its words for things, improve what is bloated or fragile, and
  write down what was taken and what changed. Never copy its addresses.

## Build, review, commit

- Build: `cmake --build build --config Game__Shipping__Win64` (the mod's DLL, the setup app and the host tests);
  tests: `ctest --test-dir build -C Game__Shipping__Win64`. Every change builds clean and passes the tests.
- `python package.py` builds, runs the tests and makes the release zip, with UE4SS's game config for this game
  (`HookLoadMap = 0`); `--nexus` makes the Nexus Mods one, without the setup app.
- The repository's checks run at every commit once turned on (`git config core.hooksPath .githooks`):
  `tools/checks/comment_style.py` (the comment rule above) and `tools/checks/code_hygiene.py` (a raw offset outside
  the facts header; a line, value or predicate that already exists; a game global rebased or a pointer hop written in a
  second place; a struct another file has; a function nothing calls). A finding is fixed, or named in the commit
  message with its reason and committed with `LSS_COMMENT_STYLE_REVIEWED=1` or `LSS_HYGIENE_REVIEWED=1` set. A fault in
  a check itself never blocks a commit. By hand: `python tools/checks/comment_style.py check src tests` and
  `python tools/checks/code_hygiene.py census .`. A game term the comment rule matches by accident goes in
  `tools/checks/comment_style_allow.txt`. For Claude Code, `.claude/settings.json` runs the same checks at each edit
  and commit.
- **Review before every commit** by a separate reviewing agent, given the diff, this file and the question "what is
  wrong with this". Fix the findings, send it back, and commit only after it approves. Small optional
  points are cheap: take them.
- **Commits are made by hand,** one step each: read the diffs, stage the files by name, build each step. Never a commit
  script. Never commit someone else's uncommitted work along with yours.
- A milestone is titled `vX.Y.Z-name: <a plain sentence about what changes for the player>`, with `kVersion` bumped in
  `src/mod.cpp` and a git tag `vX.Y.Z`.
- Nothing of the game's goes into a commit: no game files, no decompiled code, no extracted assets, no keys. Addresses
  and offsets are fine.
- Push only when the maintainer asks, but offer reminders.

## Driving the game

- Close the game before installing a DLL (it holds the file open), and close it at the end of a task.
  `python tools/game.py relaunch --deploy --label <name>` installs and starts it (`--no-launch` to install only), and
  turns on the development console the tools use (`console = on` under `[Development]`; off by default).
- Install only this mod's DLL and the setup app, never the `UE4SS.dll` this build produces.
- The tools run from the repository with Python 3.11 or later; each one's first lines say how to use it. They find
  the game through Steam's library list and their helpers where those install by default; to point them elsewhere:
  `FF7R_GAME_DIR` (the game's `End\Binaries\Win64` folder), `VIGEM_CLIENT_DLL`, `RENDERDOC_DIR`, `NGFX`,
  `LLVM_SYMBOLIZER`; `LSS_CAPTURES_DIR` and `LSS_SHOTS_DIR` (where captures and screenshots go); `LSS_MODDLL` and
  `LSS_DUMP` (the performance analysis); `IDA_DATABASES` (the databases `tools/checks/ida_db_guard.py` watches).
- Read a screenshot before every menu confirmation. Never script past a Save or Load dialog: a save was overwritten
  that way once.
- Never touch a play session someone else is running: copy the log read-only and wait.
- A launch that silently does not start is usually Steam waiting (a cloud-save conflict, a dialog, a stuck shader
  update): look, do not keep relaunching.
- Crash dumps go to `Documents\My Games\...\Saved\Crashes`. Copy `UE4SS.log` before the next launch overwrites it.
- Never move or rename the game's files or other installed copies of it.

## Documentation

- Update the documentation in the same piece of work as the code. A stale document misleads; a missing one only slows
  you down.
- Plans keep one file per topic: **State** on top, rewritten whenever things change; **History** below, dated entries
  appended and never reworded. A milestone records its version, its commit, whether it was played and what confirmed
  it.
- Research notes record, per system: the mechanism and how it was found, stock Unreal Engine versus Square Enix's code,
  the addresses per build, and a confidence marker on every claim.

## Working as an agent

- **Read before you edit,** every time. Never rely on a summary of a file you have not opened.
- **When the code and a document disagree, the code and the log are right.** Fix the document and say so.
- **Ask only for decisions.** Routine choices are yours; a choice that changes what a player experiences, a default, a
  wording a player reads, or anything hard to undo is the maintainer's. When you ask, give your recommendation first.
- **Report outcomes as they are:** failures with their output, skipped steps as skipped, unverified as unverified, and
  what you were unable to do. Never present an inference as something a person said or saw.
- **Split large mechanical work** into slices of multiple files: commit before handing a slice out, have each worker
  append to a progress log after every file, and check the result against the diff, never against the worker's report.
- **Keep the session's evidence.** Copy a reported play log before it is overwritten, record findings in the docs as you
  go, and write a decision down the moment it is made.

## Never

- Never recreate a game system that the game already provides a lever for.
- Never ship a change without its log lines, and never delete or throttle a log line to make the log quieter.
- Never call something done, working or verified without seeing it in the game and quoting its lines.
- Never put a raw offset or address outside the facts header, and never write to an offset you have not read live.
- Never treat an address as an object or a pointer without checking how the game reads it.
- Never fix one symptom of a game system and leave the system's other gaps for the next report.
- Never guess a cause, and never build on an `[ASSUMPTION]` as if it were `[CONFIRMED]`.
- Never cut dialogue, skip the game's animations, or reframe its cameras.
- Never write a path that only works for two players or for one window.
- Never commit without a review, with a script, or with someone else's work in it; never commit game files, decompiled
  code, assets or keys.
- Never edit comments or names by regex.
- Never script past a Save or Load dialog, or touch someone else's play session.

## Worked examples

**A log line.** Wrong: `LogDebug(L"result {}", value);` (what result, of what, why?). Right:
`LogDebug(L"summon seats: player {}'s menu asks whether {}: {}", seat + 1, what, answer ? L"yes (player 1's answer)" :
L"no (player 1's answer)")`, with a third wording for the branch where player 1's controller could not be asked, and
written only when that seat's answer changes.

**A fact.** Wrong: `platform::Read(manager + 0x3AC, lock);` in a module. Right: in `src/engine_facts.h`,
`inline constexpr std::uint32_t kSummonManagerEvent = 0x3AC;` beside the global's own fact, with a comment saying the
global holds a pointer to the manager and naming the one function that writes the byte; the module reads
`manager + facts::kSummonManagerEvent` through the guarded helper.

**Confirming a fix.** Wrong: "The user says summons work now, so it's fixed." Right: "Confirmed in the log: at
17:21:01 `player 2's menu asks whether the summon gauge shows: yes (player 1's answer)`; Ifrit's level loaded at
17:21:32; at 17:21:46 the summon's commands answered for player 2." Then the plan's State and History are updated, and
the version is marked as played.

**A comment.** Wrong: `// Fixed in v0.220 after the user saw a double join (0x14141F690)`. Right:
`// A pad whose player is out for now plays no one yet: its Start says the wait again, never a second join.`

**A commit title.** Wrong: `fix stuff`, `refactor scopes`. Right:
`v0.223.0-summon-seats: players 2 and 3 can summon with their own characters`.

**A research question.** Wrong: "Look into why summons don't work for player 2." Right: a numbered list. (1) Where does
the game decide a summon can be called, and which parts of that test depend on which menu asks? (2) What happens when
it is chosen: who casts, what it costs, what is tied to player 1? (3) Exactly which check fails for player 2's menu,
with the function and the field? (4) The cheapest way through the game's own path, with the entry bytes of anything to
redirect, and what to verify live.

**Porting a system.** Wrong: player 2's lock-on camera lurches, so the one camera read that lurches is scoped, and the
next report is a boss still targeting player 1. Right: a table of every read of "the player" and every shared write in
the lock-on camera's tick and the targeting it feeds, each marked handled or not, and every row built before the first
play test.
