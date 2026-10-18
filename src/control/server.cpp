#include "control/server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <format>

#pragma comment(lib, "ws2_32.lib")

namespace lss::control {

namespace {

SOCKET Sock(std::uintptr_t value) noexcept
{
    return static_cast<SOCKET>(value);
}

/// How long the thread sleeps in select between looks at the stop flag.
constexpr long kPollMilliseconds = 20;

}  // namespace

bool Server::Open(std::uint16_t port, std::wstring& error) noexcept
{
    if (listening_)
    {
        return true;
    }
    WSADATA data{};
    if (const int result = WSAStartup(MAKEWORD(2, 2), &data); result != 0)
    {
        error = std::format(L"WSAStartup failed ({})", result);
        return false;
    }
    winsock_ = true;

    const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        error = std::format(L"socket failed ({})", WSAGetLastError());
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port   = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    u_long nonBlocking = 1;
    if (bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(sock, SOMAXCONN) == SOCKET_ERROR || ioctlsocket(sock, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        error = std::format(L"port {} cannot be used ({})", port, WSAGetLastError());
        closesocket(sock);
        return false;
    }
    listen_    = static_cast<std::uintptr_t>(sock);
    listening_ = true;
    stop_      = false;
    try
    {
        thread_ = std::thread([this] { Loop(); });
    }
    catch (...)
    {
        error = L"the server thread could not be started";
        closesocket(sock);
        listen_    = 0;
        listening_ = false;
        return false;
    }
    return true;
}

void Server::Close() noexcept
{
    if (listening_)
    {
        {
            const std::lock_guard lock(mutex_);
            stop_ = true;
        }
        if (thread_.joinable())
        {
            thread_.join();
        }
        closesocket(Sock(listen_));
        listen_    = 0;
        listening_ = false;
    }
    if (winsock_)
    {
        WSACleanup();
        winsock_ = false;
    }
}

std::vector<Server::Event> Server::Drain() noexcept
{
    std::vector<Event> out;
    try
    {
        const std::lock_guard lock(mutex_);
        out.swap(events_);
    }
    catch (...)
    {
    }
    return out;
}

void Server::Send(ClientId client, std::string line) noexcept
{
    try
    {
        line += '\n';
        const std::lock_guard lock(mutex_);
        outgoing_.emplace_back(client, std::move(line));
    }
    catch (...)
    {
    }
}

void Server::Accept() noexcept
{
    const SOCKET sock = accept(Sock(listen_), nullptr, nullptr);
    if (sock == INVALID_SOCKET)
    {
        return;
    }
    u_long nonBlocking = 1;
    if (clients_.size() >= kMaxClients || ioctlsocket(sock, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        closesocket(sock);
        return;
    }
    try
    {
        clients_.push_back({nextId_++, static_cast<std::uintptr_t>(sock), {}, {}});
    }
    catch (...)
    {
        closesocket(sock);
    }
}

void Server::Read(Client& client, std::vector<Client>::iterator& it) noexcept
{
    char      buffer[4096];
    const int got = recv(Sock(client.socket), buffer, sizeof(buffer), 0);
    bool      gone = got == 0 || (got == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK);
    if (got > 0)
    {
        try
        {
            client.in.append(buffer, static_cast<std::size_t>(got));
            std::vector<Event> lines;
            for (std::size_t end; (end = client.in.find('\n')) != std::string::npos;)
            {
                std::string line = client.in.substr(0, end);
                client.in.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                lines.push_back({client.id, false, std::move(line)});
            }
            if (client.in.size() > kMaxLine)
            {
                gone = true;
            }
            const std::lock_guard lock(mutex_);
            events_.insert(events_.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
        }
        catch (...)
        {
            gone = true;
        }
    }
    if (gone)
    {
        closesocket(Sock(client.socket));
        try
        {
            const std::lock_guard lock(mutex_);
            events_.push_back({client.id, true, {}});
        }
        catch (...)
        {
        }
        it = clients_.erase(it);
        return;
    }
    ++it;
}

void Server::Loop() noexcept
{
    for (;;)
    {
        // Take the replies, and stop if asked.
        std::vector<std::pair<ClientId, std::string>> outgoing;
        {
            const std::lock_guard lock(mutex_);
            if (stop_)
            {
                break;
            }
            outgoing.swap(outgoing_);
        }
        for (auto& [id, text] : outgoing)
        {
            for (auto& client : clients_)
            {
                if (client.id == id)
                {
                    try
                    {
                        client.out += text;
                    }
                    catch (...)
                    {
                    }
                }
            }
        }

        fd_set readable, writable;
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        FD_SET(Sock(listen_), &readable);
        for (const auto& client : clients_)
        {
            FD_SET(Sock(client.socket), &readable);
            if (!client.out.empty())
            {
                FD_SET(Sock(client.socket), &writable);
            }
        }
        timeval timeout{0, kPollMilliseconds * 1000};
        if (select(0, &readable, &writable, nullptr, &timeout) == SOCKET_ERROR)
        {
            Sleep(static_cast<DWORD>(kPollMilliseconds));
            continue;
        }
        if (FD_ISSET(Sock(listen_), &readable))
        {
            Accept();
        }
        for (auto it = clients_.begin(); it != clients_.end();)
        {
            Client& client = *it;
            if (FD_ISSET(Sock(client.socket), &writable) && !client.out.empty())
            {
                const int sent = send(Sock(client.socket), client.out.data(), static_cast<int>(client.out.size()), 0);
                if (sent > 0)
                {
                    client.out.erase(0, static_cast<std::size_t>(sent));
                }
            }
            if (FD_ISSET(Sock(client.socket), &readable))
            {
                Read(client, it);  // advances it, or erases the client
            }
            else
            {
                ++it;
            }
        }
    }
    for (auto& client : clients_)
    {
        closesocket(Sock(client.socket));
    }
    clients_.clear();
}

}  // namespace lss::control
