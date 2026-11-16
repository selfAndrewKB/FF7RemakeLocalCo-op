#pragma once

// The performance counter (QueryPerformanceCounter): a fixed-rate clock from boot, the clock of the mod's own timings
// and of the engine's frame times (whose u32 counts are this counter's ticks). Any thread. Differences only.

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace lss::platform {

/// The counter now, in its ticks.
inline std::int64_t Ticks() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

/// The counter's ticks per second (fixed for the process; read once, under the language's own lock for a local static).
inline std::int64_t TicksPerSecond() noexcept
{
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}

}  // namespace lss::platform
