#pragma once

// The mod's log: one line per event, written to UE4SS.log with a "[LocalSplitScreen]" prefix.
//
// Every line has a level. The log writes the levels up to the current one (Debug at start; the console command
// `log-level` changes it at run time):
//   Error, Warning  something failed, or the mod stood a feature aside;
//   Info            what the mod verified at startup, players joining and leaving, mode switches, state changes
//                   that happen a few times a minute at most;
//   Debug           per-event diagnostics and periodic status lines;
//   Trace           per-frame and per-input reports.
// A line above the current level costs one comparison: its text is never formatted. Code that builds an
// expensive argument for a Debug or Trace line checks Enabled() first.
//
// Formatting uses std::format, so the format string is checked at compile time:
//     platform::Log(L"player {} joined", index);              // Info
//     platform::LogDebug(L"hit by {}", name);                 // Debug
//     if (platform::Enabled(platform::Level::Trace)) { ... }  // before building an expensive argument

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace lss::platform {

enum class Level : std::uint8_t
{
    Error,
    Warning,
    Info,
    Debug,
    Trace,
};

/// The most detailed level written (Debug at start). Any thread.
void  SetLevel(Level level) noexcept;
Level CurrentLevel() noexcept;

/// The level's name ("error" .. "trace").
const wchar_t* LevelName(Level level) noexcept;

namespace detail {
extern std::atomic<std::uint8_t> g_level;
}

/// True if a line of `level` is written now.
inline bool Enabled(Level level) noexcept
{
    return static_cast<std::uint8_t>(level) <= detail::g_level.load(std::memory_order_relaxed);
}

/// Writes one already-formatted line (the prefix and the newline are added here), whatever the level.
void WriteLine(std::wstring_view line) noexcept;

/// Writes one line of `level`, if that level is enabled. Error, Warning, Debug and Trace lines carry the level's
/// name after the prefix; Info lines carry none.
template <class... Args>
void LogAt(Level level, std::wformat_string<Args...> fmt, Args&&... args) noexcept
{
    if (!Enabled(level))
    {
        return;
    }
    try
    {
        std::wstring line = std::format(fmt, std::forward<Args>(args)...);
        if (level != Level::Info)
        {
            line.insert(0, std::format(L"[{}] ", LevelName(level)));
        }
        WriteLine(line);
    }
    catch (...)
    {
        // Formatting can only fail by running out of memory. A lost log line must never take the game down.
        WriteLine(L"(a log line could not be formatted)");
    }
}

template <class... Args>
void Log(std::wformat_string<Args...> fmt, Args&&... args) noexcept
{
    LogAt(Level::Info, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void LogError(std::wformat_string<Args...> fmt, Args&&... args) noexcept
{
    LogAt(Level::Error, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void LogWarning(std::wformat_string<Args...> fmt, Args&&... args) noexcept
{
    LogAt(Level::Warning, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void LogDebug(std::wformat_string<Args...> fmt, Args&&... args) noexcept
{
    LogAt(Level::Debug, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void LogTrace(std::wformat_string<Args...> fmt, Args&&... args) noexcept
{
    LogAt(Level::Trace, fmt, std::forward<Args>(args)...);
}

}  // namespace lss::platform
