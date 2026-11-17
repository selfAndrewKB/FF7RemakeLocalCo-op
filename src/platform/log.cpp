#include "platform/log.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <mutex>

namespace lss::platform {

namespace detail {
std::atomic<std::uint8_t> g_level{static_cast<std::uint8_t>(Level::Debug)};  // Debug by default; per-frame lines are Trace
}

void SetLevel(Level level) noexcept
{
    detail::g_level.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed);
}

Level CurrentLevel() noexcept
{
    return static_cast<Level>(detail::g_level.load(std::memory_order_relaxed));
}

const wchar_t* LevelName(Level level) noexcept
{
    switch (level)
    {
    case Level::Error: return L"error";
    case Level::Warning: return L"warning";
    case Level::Info: return L"info";
    case Level::Debug: return L"debug";
    case Level::Trace: return L"trace";
    }
    return L"?";
}

void WriteLine(std::wstring_view line) noexcept
{
    // UE4SS formats with "{}" placeholders, so the text is passed as an argument, never as the
    // format string: a '{' in a message would otherwise be read as a placeholder.
    // Warning level is the one UE4SS writes to its log file in shipping builds.
    // The sink allocates and formats, so it can throw. This function is noexcept, and an escaping
    // exception would end the game over a log line; a lost line is the lesser failure.
    // UE4SS's output devices are not synchronised, and the mod logs from the render thread and the
    // console's socket thread too, so lines are written one at a time.
    static std::mutex mutex;
    try
    {
        const std::lock_guard lock(mutex);
        RC::Output::send<RC::LogLevel::Warning>(STR("[LocalSplitScreen] {}\n"), line);
    }
    catch (...)
    {
    }
}

}  // namespace lss::platform
