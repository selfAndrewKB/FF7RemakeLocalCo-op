#pragma once

// Control scripts from files: a way to drive the mod, and through it the game, without keys or the game
// window's focus (adapted from Rebirth's control module).
//
// Folders beside the mod's `dlls` folder:
//   Mods/LocalSplitScreen/control/inbox/   scripts to run (*.txt, UTF-8); write under another extension and
//                                          rename, so a half-written file is never read
//   Mods/LocalSplitScreen/control/done/    each script once taken, and its <name>.result.txt
// Scripts run one at a time, lowest file name first. A script with an invalid line is rejected whole. Each
// completed command is appended to the result file at once (and logged); the last line is
// `finished: ok|failed|rejected`. The first failing command stops the script and releases its stick holds.
//
// The language, one command per line ('#' starts a comment):
//   join | leave                        add or remove player 2 (as Ctrl+F7 / Ctrl+F8)
//   seat | unseat                       player 2 takes Aerith, or gives her back (the Ctrl+F9 probe)
//   stick <player> <lx> <ly> <rx> <ry> <ticks>
//                                       hold a player's sticks (-1..1) for a number of ticks, in place of the
//                                       pad; runs alongside the commands after it; needs player 2 in
//   wait <ticks>                        do nothing for a number of ticks
//   console <command>                   an engine console command
//   status                              log the players and each one's input, movement and camera counts
//   note <text>                         write a marker line to the log
//   quit                                quit the game through the engine (player 2 must be out)
//
// Game thread only: Tick runs from the engine-tick callback, before the world ticks.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lss::control {

/// What a command did; `detail` says why when it failed.
struct Outcome
{
    bool         ok = true;
    std::wstring detail;
};

/// The mod as a script sees it. Game thread only.
class Target
{
public:
    virtual ~Target() = default;

    virtual Outcome Join()                                          = 0;
    virtual Outcome Leave()                                         = 0;
    virtual Outcome Seat(bool seated)                               = 0;
    virtual Outcome HoldStick(std::int32_t player, const float (&axes)[4]) = 0;
    virtual void    ReleaseStick(std::int32_t player)               = 0;
    virtual Outcome Console(const std::wstring& command)            = 0;
    virtual Outcome Status()                                        = 0;
    virtual Outcome Quit()                                          = 0;
};

enum class CommandKind
{
    Join,
    Leave,
    Seat,
    Unseat,
    Stick,
    Wait,
    Console,
    Status,
    Note,
    Quit,
};

struct Command
{
    CommandKind   kind    = CommandKind::Note;
    std::int32_t  player  = 0;   // stick: 1 or 2
    float         axes[4] = {};  // stick: LX, LY, RX, RY
    std::uint32_t ticks   = 0;   // stick, wait
    std::wstring  text;          // console, note
};

/// The longest a stick hold or wait may last: 10 minutes at 60 ticks per second.
inline constexpr std::uint32_t kMaxTicks = 36'000;

struct ParsedLine
{
    std::optional<Command> command;  // empty for a blank line or a comment, or on an error
    std::wstring           error;    // set when the line is not a valid command
};

ParsedLine   ParseLine(std::wstring_view line);
std::wstring Describe(const Command& command);

class CommandChannel
{
public:
    /// Creates the folders next to the loaded DLL. False (logged) if they cannot be made; Tick then does nothing.
    bool Open() noexcept;

    /// Runs the current script's next tick, or looks for a new script every few ticks.
    void Tick(Target& target) noexcept;

private:
    struct Line
    {
        std::uint32_t number = 0;
        Command       command;
    };
    struct Running
    {
        std::vector<Line>            lines;
        std::size_t                  next      = 0;
        std::uint32_t                waitTicks = 0;
        std::uint32_t                holdTicks[2] = {};
        std::filesystem::path        results;
        std::wstring                 name;
    };

    void StartNext();
    void Step(Target& target);
    void Finish(Target& target, bool failed);
    void Append(const std::wstring& line) noexcept;

    std::filesystem::path  inbox_;
    std::filesystem::path  done_;
    bool                   open_ = false;
    std::uint64_t          tick_ = 0;
    std::optional<Running> running_;
};

}  // namespace lss::control
