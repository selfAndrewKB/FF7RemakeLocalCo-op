#include "platform/log.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <mutex>

namespace lss::platform {

void WriteLine(std::wstring_view line) noexcept
{
    // UE4SS formats with "{}" placeholders, so the text is passed as an argument, never as the
    // format string: a '{' in a message would otherwise be read as a placeholder.
    // Warning level is the one UE4SS writes to its log file in shipping builds.
    // The sink allocates and formats, so it can throw. This function is noexcept, and an escaping
    // exception would end the game over a log line; a lost line is the lesser failure.
    // UE4SS's output devices are not synchronised, and the mod logs from the render thread too
    // (render/ao_origin), so lines are written one at a time.
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
