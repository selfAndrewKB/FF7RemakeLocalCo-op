#pragma once

// The guard: the setup app started by the mod with `--guard <handle> <record>` before it moves any monitor
// (views/display_align). No window: one wait on the game, the monitors put back if that game's record is still there when the
// game ends, one line in `<record>.log` saying what it found, and gone. It runs only while the game does.

namespace lss::setup::guard {

/// True when the command line asked for a guard, with `exitCode` set: 0 done, 1 the monitors could not be put back, 2 the
/// arguments were not what the mod passes (nothing was changed): a record under any name but the mod's
/// (views/record_file kName) is one of those, so a stray argument can name no other file. False for any other command
/// line: the app shows its window.
bool Run(int& exitCode);

}  // namespace lss::setup::guard
