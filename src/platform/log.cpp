#include "platform/log.h"

#include <DynamicOutput/DynamicOutput.hpp>

namespace lss::platform {

void WriteLine(std::wstring_view line) noexcept
{
    // UE4SS formats with "{}" placeholders, so the text is passed as an argument, never as the
    // format string: a '{' in a message would otherwise be read as a placeholder.
    // Warning level is the one UE4SS writes to its log file in shipping builds.
    // The sink allocates and formats, so it can throw. This function is noexcept, and an escaping
    // exception would end the game over a log line; a lost line is the lesser failure.
    try
    {
        RC::Output::send<RC::LogLevel::Warning>(STR("[LocalSplitScreen] {}\n"), line);
    }
    catch (...)
    {
    }
}

}  // namespace lss::platform
