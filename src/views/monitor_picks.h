#pragma once

// The monitors the player picked for the row, as the settings file keeps them, and the row they give. The mod's span
// (views/window_span) and the setup app (setup/) both ask here, so the app's picture is what the game does.
//
// Picks are saved by identity, since a DISPLAYn number changes when Windows renumbers its outputs. A monitor's device path
// names its port, and a streaming host's virtual monitor gets a new path each time it is made (a SudoVDA monitor: ...UID256, then
// ...UID257 for the same monitor made again), so a pick also keeps the monitor's model and name to find it by.
//
// The rule for named monitors: the row is made only of those that are connected, in the order named, and never of others.
// With fewer than two connected there is no row and the game waits for them in one window. Only a settings file that names
// no monitor gets a row the desktop already has (monitor_row::Choose).
//
// This module decides from lists; it touches no OS and no engine, so it is unit-tested on the host
// (tests/monitor_picks_test.cpp).

#include "views/column_rules.h"
#include "views/monitor_row.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lss::views::monitor_picks {

/// The most monitors a row takes: one per column the views build.
inline constexpr std::int32_t kMostInRow = column_rules::kMaxColumns;

/// What finds a monitor again after its path changed: its model (from the device path) and its own name, in characters a
/// settings word can hold. Empty when the path has no model in it. `path` is lower-case, as monitor_list gives it.
std::wstring IdentityKey(std::wstring_view path, std::wstring_view name);

/// One monitor as the settings word keeps it.
std::wstring PickWord(std::wstring_view id, std::wstring_view key);

/// The settings word: pick words in the row's order, joined.
std::wstring NamesWord(std::span<const std::wstring> pickWords);

/// The monitors a settings word names, as they stand on the desktop now.
struct Picks
{
    std::int32_t             named = 0;  // the monitors the word names, connected or not
    std::int32_t             away  = 0;  // those of them that are not connected
    std::vector<std::size_t> found;      // the connected ones the row takes, in the word's order: at most kMostInRow
};

/// The picks a settings word finds on the desktop now. An exact path wins; then a connected monitor of the same identity
/// that no other pick took, in list order, as two monitors of one identity are interchangeable. `ids` and `keys` hold one
/// lower-case path and one IdentityKey per monitor; the word is compared as the settings file reads it, lower-cased.
Picks FindPicks(std::span<const std::wstring> ids, std::span<const std::wstring> keys, std::wstring_view word);

/// The columns the named monitors give: one per connected one; 0 when fewer than two are connected, or none is named.
std::int32_t PickedColumns(const Picks& picks) noexcept;

/// The row of `columns` columns. With monitors named: those connected, the first `columns` of them, moved into a row wherever
/// they sit (monitor_row::Arrange); fewer than two connected is no row, with the reason. With none named: a row the desktop
/// has, around `monitors[gameMonitor]` when that qualifies (monitor_row::Choose).
monitor_row::Choice Row(std::span<const monitor_row::Monitor> monitors, const Picks& picks, std::size_t gameMonitor, std::int32_t columns);

/// The row with the most columns, up to `most`: what the game spans when it is asked for every column. With none named, a
/// desktop whose longest row is shorter than `most` gives that row.
monitor_row::Choice WidestRow(std::span<const monitor_row::Monitor> monitors, const Picks& picks, std::size_t gameMonitor, std::int32_t most);

}  // namespace lss::views::monitor_picks
