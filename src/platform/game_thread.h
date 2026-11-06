#pragma once

// The game thread: the one the engine ticks on. The mod's engine tick marks it first thing, every tick, and whatever the
// game calls into the mod from elsewhere (the render thread, a worker, UE4SS's own thread at an unload) asks here before it
// touches what only the game thread may.

#include <atomic>

namespace lss::platform {

/// The calling thread is the game thread. From the mod's engine tick, first thing, every tick.
void MarkGameThread() noexcept;

/// The calling thread is the game thread. False until the first engine tick.
bool OnGameThread() noexcept;

/// `thread` is the game thread's id. False until the first engine tick.
bool IsGameThread(unsigned long thread) noexcept;

/// A module's own mark that it has run on the game thread, for a module whose calls from the game count only from then on:
/// one that ticks only once installed, or does nothing until a player joins. Until Set, nothing is on the game thread for it.
class GameThreadMark
{
public:
    /// From the game thread only.
    void Set() noexcept { set_.store(true, std::memory_order_relaxed); }

    /// Set, and the calling thread is the game thread.
    bool Here() const noexcept { return set_.load(std::memory_order_relaxed) && OnGameThread(); }

    /// Set, and `thread` is the game thread's id.
    bool Is(unsigned long thread) const noexcept { return set_.load(std::memory_order_relaxed) && IsGameThread(thread); }

private:
    std::atomic<bool> set_{false};
};

}  // namespace lss::platform
