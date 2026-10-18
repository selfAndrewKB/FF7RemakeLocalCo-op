#include "control/remote.h"

#include "platform/log.h"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::control {

namespace {

using platform::Level;
using platform::Log;

std::wstring FromUtf8(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string ToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return {};
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), bytes.data(), length, nullptr, nullptr);
    return bytes;
}

/// Replies about actions are worth the log at Info; the chatter of queries and echoes is Debug.
Level ReplyLevel(const std::wstring& reply)
{
    return reply.starts_with(L"value ") || reply.starts_with(L"help ") || reply.starts_with(L"note ") ||
                   reply.starts_with(L"ok ") && (reply.find(L" get ") != std::wstring::npos ||
                                                 reply.ends_with(L" status") || reply.find(L" help") != std::wstring::npos)
               ? Level::Debug
               : Level::Info;
}

}  // namespace

bool Remote::Open() noexcept
try
{
    static const std::vector<std::wstring> kLevels{L"error", L"warning", L"info", L"debug", L"trace"};
    Register({.name  = L"log-level",
              .shape = Shape::Word,
              .words = kLevels,
              .help  = L"the most detailed log level written (info at start)",
              .set   = [](const Value& value) {
                  const auto& word = std::get<std::wstring>(value);
                  for (std::size_t i = 0; i < kLevels.size(); ++i)
                  {
                      if (kLevels[i] == word)
                      {
                          platform::SetLevel(static_cast<Level>(i));
                      }
                  }
                  return Outcome{};
              },
              .get = [] { return Value{std::wstring(platform::LevelName(platform::CurrentLevel()))}; }});
    Register({.name  = L"tick",
              .shape = Shape::Integer,
              .min   = 0,
              .max   = INT64_MAX,
              .help  = L"the console's tick count since it opened",
              .get   = [this] { return Value{static_cast<std::int64_t>(tick_)}; }});
    for (const auto& name : Global().Rejected())
    {
        platform::LogWarning(L"console: the command '{}' could not be registered (a duplicate or malformed entry)", name);
    }

    std::wstring error;
    if (!server_.Open(kPort, error))
    {
        platform::LogWarning(L"console: not listening: {}", error);
        return false;
    }
    open_ = true;
    Log(L"console: listening on 127.0.0.1:{} ({} commands; tools/game.py shell)", kPort, Global().Entries().size());
    return true;
}
catch (...)
{
    platform::LogWarning(L"console: could not open");
    return false;
}

void Remote::Tick() noexcept
{
    if (!open_)
    {
        return;
    }
    ++tick_;
    try
    {
        for (auto& event : server_.Drain())
        {
            if (event.closed)
            {
                runner_.ClientClosed(event.client);
                platform::LogDebug(L"console: client {} left", event.client);
            }
            else
            {
                const std::wstring line = FromUtf8(event.line);
                platform::LogDebug(L"console: client {}: {}", event.client, line);
                runner_.Submit(event.client, line);
            }
        }
        runner_.Tick(tick_);
        for (const auto& reply : runner_.TakeReplies())
        {
            platform::LogAt(ReplyLevel(reply.text), L"console: to {}: {}", reply.client, reply.text);
            server_.Send(reply.client, ToUtf8(reply.text));
        }
    }
    catch (...)
    {
        platform::LogWarning(L"console: an exception stopped it");
        Close();
    }
}

void Remote::Close() noexcept
{
    if (!open_)
    {
        return;
    }
    open_ = false;
    try
    {
        runner_.Shutdown();
        for (const auto& reply : runner_.TakeReplies())
        {
            server_.Send(reply.client, ToUtf8(reply.text));
        }
    }
    catch (...)
    {
    }
    server_.Close();
    Log(L"console: closed");
}

}  // namespace lss::control
