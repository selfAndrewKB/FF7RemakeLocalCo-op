#pragma once

// N floats one thread writes and others read (Boehm's seqlock): the writer bumps the version to odd, stores the
// values, bumps it to even; a reader retries while it sees an odd version or a version that changed under it. No
// locks, so it serves the audio and SEAD threads (audio/listeners). Taken from Rebirth's platform/seqlock.h as is.
// Tested on the host (tests/listener_rules_test.cpp).

#include <array>
#include <atomic>
#include <cstddef>

namespace lss::platform {

template <std::size_t N>
struct SeqFloats
{
    std::atomic<unsigned>             version{0};
    std::array<std::atomic<float>, N> values{};

    /// One writer thread only.
    void Publish(const std::array<float, N>& in) noexcept
    {
        const auto v = version.load(std::memory_order_relaxed);
        version.store(v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < N; ++i)
        {
            values[i].store(in[i], std::memory_order_relaxed);
        }
        version.store(v + 2, std::memory_order_release);
    }

    /// False before the first publish, or when four attempts all crossed a publish.
    bool Read(std::array<float, N>& out) const noexcept
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const auto before = version.load(std::memory_order_acquire);
            if ((before & 1u) != 0)
            {
                continue;
            }
            for (std::size_t i = 0; i < N; ++i)
            {
                out[i] = values[i].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (version.load(std::memory_order_relaxed) == before)
            {
                return before != 0;
            }
        }
        return false;
    }
};

}  // namespace lss::platform
