#include "render/family_now.h"

#include "engine_facts.h"
#include "platform/guarded.h"

#include <atomic>

namespace lss::render::family_now {

namespace {

/// Index (low dword, also read as a plain dword by machine code) and count in one word, so they are never read torn.
constexpr std::uint64_t Pack(std::int32_t index, std::int32_t count) noexcept
{
    return static_cast<std::uint32_t>(index) | std::uint64_t{static_cast<std::uint32_t>(count)} << 32;
}

std::atomic<std::uint64_t> g_place{Pack(-1, 0)};
std::atomic<const void*>   g_state{nullptr};
static_assert(sizeof(g_place) == sizeof(std::uint64_t) && std::atomic<std::uint64_t>::is_always_lock_free);

}  // namespace

Family Current() noexcept
{
    const std::uint64_t place = g_place.load();
    const auto          index = static_cast<std::int32_t>(static_cast<std::uint32_t>(place));
    if (index < 0)
    {
        return {};
    }
    return {index, static_cast<std::int32_t>(static_cast<std::uint32_t>(place >> 32)), g_state.load()};
}

Family For(const void* view) noexcept
{
    const Family family = Current();
    const void*  state  = nullptr;
    if (family.index < 0 || family.state == nullptr || view == nullptr ||
        !platform::CopyGuarded(&state, static_cast<const std::uint8_t*>(view) + facts::kViewState, sizeof(state)) ||
        state != family.state)
    {
        return {};
    }
    return family;
}

const std::int32_t* IndexAddress() noexcept
{
    return reinterpret_cast<const std::int32_t*>(&g_place);  // little-endian: the low dword is the index
}

void Set(Family family) noexcept
{
    // Sequentially consistent: a fault's clear and a task's set on two threads end with the clear (family_boundary.cpp).
    // A place is published after its state; a clear takes the place first.
    if (family.index >= 0)
    {
        g_state.store(family.state);
        g_place.store(Pack(family.index, family.count));
    }
    else
    {
        g_place.store(Pack(-1, 0));
        g_state.store(nullptr);
    }
}

}  // namespace lss::render::family_now
