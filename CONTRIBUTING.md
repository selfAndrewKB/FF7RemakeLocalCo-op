# Contributing

Read `AGENTS.md` first: it is how this mod is built, and changes are reviewed against it.

Contributions are licensed under the project's licence (`LICENSE`, with the permission in `LICENSE-EXCEPTION`).

## Sending a change

- One thing per change, built clean, with the tests passing.
- Its log lines, and the lines that show it working in the game, quoted in the pull request.
- Which player counts and screen layouts you tested, and which you could not.
- If it rests on new research into the game's code: what you found, how, and for which game version.

Never commit game files, extracted assets, decompiled code or keys. Addresses and offsets are fine.

## Reporting a bug

Attach `UE4SS.log` from the game's `End\Binaries\Win64\ue4ss\` folder before launching the game again (the next launch
replaces it), and say:

- the game and mod versions;
- how many players, and one window or a row of monitors;
- what happened, and roughly when in the session.

Think twice before attaching game files or extracted assets.
