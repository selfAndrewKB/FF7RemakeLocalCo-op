#pragma once

// The mod's log: one line per event, written to UE4SS.log with a "[LocalSplitScreen]" prefix.
//
// The product logs what it verified at startup, when players join or leave, and anything that
// made it stand aside. Nothing is logged per frame.
//
// Formatting uses std::format, so the format string is checked at compile time:
//     platform::Log(L"player {} joined", index);

#include <format>
#include <string_view>
#include <utility>

namespace lss::platform {

/// Writes one already-formatted line (the prefix and the newline are added here).
void WriteLine(std::wstring_view line) noexcept;

template <class... Args>
void Log(std::wformat_string<Args...> fmt, Args&&... args) noexcept
{
    try
    {
        WriteLine(std::format(fmt, std::forward<Args>(args)...));
    }
    catch (...)
    {
        // Formatting can only fail by running out of memory. A lost log line must never take
        // the game down with it.
        WriteLine(L"(a log line could not be formatted)");
    }
}

}  // namespace lss::platform
