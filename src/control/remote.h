#pragma once

// The console: the socket server, the runner and the registry joined on the game thread. Open after every
// module has registered its commands (the registry's refusals are logged then); Tick from the engine tick,
// before the world ticks; Close before the mod unloads. Also registers the console's own commands: `log-level`
// and the `tick` reading.
//
// Driving it: `python tools/game.py shell` (or `-c "<line>"`, `run <file>`), a line per command on
// 127.0.0.1:kPort; replies are the runner's lines (control/runner.h).

#include "control/runner.h"
#include "control/server.h"

#include <cstdint>

namespace lss::control {

class Remote
{
public:
    static constexpr std::uint16_t kPort = 4418;

    explicit Remote(Holds& holds) noexcept : runner_(Global(), holds) {}

    /// Listens on kPort. False (logged) if it cannot; Tick then does nothing.
    bool Open() noexcept;

    /// Hands the lines received since the last tick to the runner, advances it, and sends its replies.
    void Tick() noexcept;

    /// Drops every queued command, releases every hold and closes the socket.
    void Close() noexcept;

    std::uint64_t CurrentTick() const noexcept { return tick_; }

private:
    Server        server_;
    Runner        runner_;
    std::uint64_t tick_ = 0;
    bool          open_ = false;
};

}  // namespace lss::control
