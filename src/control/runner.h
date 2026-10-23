#pragma once

// Runs the console's commands against the registry. Game thread only; pure logic with no engine access, tested
// on the host (tests/control_runner_test.cpp).
//
// Each client has its own queue, so a connection is its own script: its actions and switches run in order, a
// bounded number per tick; its `wait` and `wait-until` hold only its own queue back; its `hold`s run alongside the
// commands after them until their ticks run out. Queries (get, status, help, note) answer where they stand: at once
// when the client has nothing pending, otherwise in turn behind its earlier commands (a status after a wait-until
// reads the state after the wait; a note marks the log where it stands in the script). A line that does not parse
// is rejected at once, out of turn, and stops nothing. A failure drops the
// commands the client queued behind it and releases its holds: a script, sent whole, stops at its first failure;
// a shell, which sends one line at a time, just sees the failure. A client that goes away drops its queue and
// releases its holds, so a later player on the same pad never inherits one. `abort` does that for every client.
// Two clients acting on the game at once interleave by tick, as two shells would.
//
// Every reply is one line that starts with a word:
//   ok <n> <command> [-- <detail>]      the client's n-th line completed (a wait: when it ended)
//   failed <n> <command> -- <reason>
//   rejected <n> <line> -- <reason>     the line did not parse; nothing ran
//   dropped <n> <command> -- <reason>   queued, never run (after a failure, an abort, or the client left)
//   value <name> <value>                from get and status
//   help <name> <shape> -- <text>       from help
//   note <n> <text>                     the note, echoed

#include "control/registry.h"
#include "control/script.h"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace lss::control {

/// The input holds the mod can make, injected at its own per-pad layer so a scripted hold takes a real pad's path.
class Holds
{
public:
    virtual ~Holds() = default;

    virtual Outcome HoldStick(std::int32_t player, const float (&axes)[4]) = 0;
    virtual void    ReleaseStick(std::int32_t player)                      = 0;
};

using ClientId = std::uint32_t;

struct Reply
{
    ClientId     client = 0;
    std::wstring text;
};

class Runner
{
public:
    /// A queue without waits still spreads over ticks, so it never stalls one frame for long.
    static constexpr std::size_t kMaxCommandsPerTick = 32;

    Runner(const Registry& registry, Holds& holds) noexcept : registry_(registry), holds_(holds) {}

    /// One line from a client: parsed now (a bad line is rejected at once), a query answered now if the client has
    /// nothing pending (else queued behind it), an action queued, an abort acted on. Replies go to TakeReplies.
    void Submit(ClientId client, std::wstring_view line);

    /// The client went away: its queue is dropped and its holds released.
    void ClientClosed(ClientId client);

    /// Advances every client's queue by one tick: holds count down, waits end, queued commands run.
    void Tick(std::uint64_t tick);

    /// Releases every hold and drops every queue (the console is closing).
    void Shutdown();

    std::vector<Reply> TakeReplies();

    bool Idle() const noexcept;

private:
    struct Queued
    {
        std::uint32_t line = 0;
        Command       command;
    };
    struct Client
    {
        ClientId           id        = 0;
        std::uint32_t      lines     = 0;  // the last line number given
        std::deque<Queued> queue;
        bool               waiting   = false;  // the head is a wait that has begun
        std::uint32_t      waitTicks = 0;
    };
    struct Hold
    {
        ClientId      client = 0;
        std::int32_t  player = 0;
        std::uint32_t ticks  = 0;
    };

    Client& Find(ClientId client);
    void    Send(ClientId client, std::wstring text);
    void    Answer(const Client& client, std::uint32_t line, const Command& command);
    void    Advance(Client& client);
    bool    Run(Client& client);  // the head command; false if the queue must stop for this tick (a wait began)
    bool    ConditionHolds(const Command& command) const;
    void    Fail(Client& client, const Queued& failed, const std::wstring& reason);
    void    DropQueue(Client& client, std::wstring_view reason);
    void    ReleaseHolds(ClientId client, bool everyClient);

    const Registry&     registry_;
    Holds&              holds_;
    std::vector<Client> clients_;
    std::vector<Hold>   held_;
    std::vector<Reply>  replies_;
};

}  // namespace lss::control
