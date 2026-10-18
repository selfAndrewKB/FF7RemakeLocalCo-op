#pragma once

// The console's socket: a listening TCP socket on 127.0.0.1 and one thread that reads lines from every client
// into a queue and writes replies out. The game thread drains the queue each tick and never touches a socket.
// Bound to the loopback address only, so nothing off this machine can reach it. Bytes in and out are UTF-8;
// a line ends with '\n' (a '\r' before it is dropped).

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lss::control {

class Server
{
public:
    using ClientId = std::uint32_t;

    struct Event
    {
        ClientId    client = 0;
        bool        closed = false;  // the client went away (line is empty)
        std::string line;            // one line, without its newline
    };

    Server() = default;
    ~Server() { Close(); }
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    /// Listens on the port and starts the thread. False (with the reason) if the port cannot be bound.
    bool Open(std::uint16_t port, std::wstring& error) noexcept;

    /// Stops the thread and closes every socket. Safe to call twice.
    void Close() noexcept;

    bool IsOpen() const noexcept { return listening_; }

    /// The lines and closures received since the last call, in order. Game thread.
    std::vector<Event> Drain() noexcept;

    /// Queues one reply line (the newline is added) for the thread to write. Any thread.
    void Send(ClientId client, std::string line) noexcept;

    /// The most clients served at once; a further connection is refused.
    static constexpr std::size_t kMaxClients = 8;

    /// A line longer than this closes its client (a wrong protocol, not a script).
    static constexpr std::size_t kMaxLine = 64 * 1024;

private:
    struct Client
    {
        ClientId      id = 0;
        std::uintptr_t socket = 0;  // SOCKET
        std::string   in;           // bytes received, not yet a whole line
        std::string   out;          // bytes to send
    };

    void Loop() noexcept;
    void Accept() noexcept;
    void Read(Client& client, std::vector<Client>::iterator& it) noexcept;

    std::uintptr_t      listen_      = 0;
    bool                listening_   = false;
    bool                winsock_     = false;
    ClientId            nextId_      = 1;
    std::thread         thread_;
    std::vector<Client> clients_;   // thread only
    std::mutex          mutex_;      // guards events_, outgoing_, stop_
    std::vector<Event>  events_;
    std::vector<std::pair<ClientId, std::string>> outgoing_;
    bool                stop_        = false;
};

}  // namespace lss::control
