#include "platform/game_thread.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace lss::platform {

namespace {

static_assert(sizeof(DWORD) == sizeof(unsigned long), "a thread id as the header's callers pass it");
std::atomic<unsigned long> g_gameThread{0};  // 0 until the first engine tick; no thread has that id

}  // namespace

void MarkGameThread() noexcept
{
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
}

bool OnGameThread() noexcept
{
    return IsGameThread(GetCurrentThreadId());
}

bool IsGameThread(unsigned long thread) noexcept
{
    return thread != 0 && thread == g_gameThread.load(std::memory_order_relaxed);
}

}  // namespace lss::platform
