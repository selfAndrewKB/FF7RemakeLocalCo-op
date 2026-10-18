#pragma once

// The control language: one command per line, UTF-8, '#' starts a comment. Registered commands (control/registry.h)
// take their one argument in the entry's shape; the verbs below belong to the runner itself.
//
//   <name> [<argument>]                  a registered action or switch (join, seat, bloom-origin on, showflags 100A)
//   get <name>                           the current value of a registered switch or reading
//   status                               every readable value, one `value` line each
//   help [<name>]                        every command with its shape and help, or one
//   wait <ticks>                         hold the queue back for a number of ticks
//   wait-until <name> <op> <value> [<ticks>]
//                                        hold the queue back until a reading compares true (ops: == != < > <= >=;
//                                        the value in the entry's shape), or fail after the timeout (600 ticks by
//                                        default)
//   hold <player> stick <lx> <ly> <rx> <ry> <ticks>
//                                        hold a player's sticks (-1..1) in place of the pad for a number of ticks;
//                                        runs alongside the commands after it
//   note <text>                          write a marker line to the log
//   abort                                stop the queue now: queued commands dropped, holds released
//
// Parsing is pure and tested on the host (tests/control_script_test.cpp).

#include "control/registry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lss::control {

enum class Verb : std::uint8_t
{
    Entry,      // a registered command: `name` and `value`
    Get,        // `name`
    Status,
    Help,       // `name` optional
    Wait,       // `ticks`
    WaitUntil,  // `name`, `compare`, `value`, `ticks` (the timeout)
    Hold,       // `player`, `axes`, `ticks`
    Note,       // `text`
    Abort,
};

enum class Compare : std::uint8_t
{
    Equal,
    NotEqual,
    Less,
    Greater,
    LessOrEqual,
    GreaterOrEqual,
};

struct Command
{
    Verb          verb    = Verb::Note;
    std::wstring  name;             // Entry, Get, Help, WaitUntil: the registered name
    Value         value;            // Entry: the argument; WaitUntil: the value compared against
    Compare       compare = Compare::Equal;
    std::uint32_t ticks   = 0;      // Wait, WaitUntil, Hold
    std::int32_t  player  = 0;      // Hold: 1 or 2
    float         axes[4] = {};     // Hold: LX, LY, RX, RY
    std::wstring  text;             // Note
};

/// The longest a hold, wait or wait-until may last: 10 minutes at 60 ticks per second.
inline constexpr std::uint32_t kMaxTicks = 36'000;

/// A wait-until's timeout when the line gives none.
inline constexpr std::uint32_t kDefaultWaitUntilTicks = 600;

struct ParsedLine
{
    std::optional<Command> command;  // empty for a blank line or a comment, or on an error
    std::wstring           error;    // set when the line is not a valid command
};

/// Parses one line against the registry. The registry is read, never kept: the command stores names.
ParsedLine ParseLine(const Registry& registry, std::wstring_view line);

/// The command as a script would write it, for replies and the log.
std::wstring Describe(const Registry& registry, const Command& command);

/// The op as written (`==` and so on).
std::wstring_view CompareText(Compare compare) noexcept;

/// The shape as help shows it: "", "on|off", "<0..36000>", "<hex up to FFFF>", "a|b|c", "<0.5..2>", "<text>".
std::wstring ShapeText(const Entry& entry);

}  // namespace lss::control
