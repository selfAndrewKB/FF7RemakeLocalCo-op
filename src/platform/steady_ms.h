#pragma once

// The steady clock in milliseconds, for the modules' timeouts and rate windows. Pure; any thread.

#include <chrono>
#include <cstdint>

namespace lss::platform {

inline std::int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace lss::platform
