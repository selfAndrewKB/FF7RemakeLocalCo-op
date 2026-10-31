#include "render/view_places.h"

#include "engine_facts.h"
#include "platform/guarded.h"
#include "render/family_split.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace lss::render::view_places {

namespace {

// Each place's view state, and how many places are listed (-1 before the first publish). Each entry is read alone:
// a reader between two stores of a publish sees an entry of either list, which only a join or leave changes.
std::array<std::atomic<const void*>, family_split::kViewsMax> g_states{};
std::atomic<std::int32_t>                                     g_count{-1};

const void* StateOf(const void* view) noexcept
{
    const void* state = nullptr;
    return view != nullptr && platform::CopyGuarded(&state, static_cast<const std::uint8_t*>(view) + facts::kViewState, sizeof(state))
               ? state
               : nullptr;
}

}  // namespace

void Publish(const std::uint8_t* family) noexcept
{
    const void** views = nullptr;
    std::int32_t count = 0;
    if (family == nullptr || !platform::Read(family + facts::kFamilyViews, views) ||
        !platform::Read(family + facts::kFamilyViewCount, count) || views == nullptr || count < 1 || count > family_split::kViewsMax)
    {
        return;  // kept as it was: a family the split also leaves whole
    }
    for (std::int32_t k = 0; k < count; ++k)
    {
        const void* view = nullptr;
        platform::Read(views + k, view);
        g_states[static_cast<std::size_t>(k)].store(StateOf(view), std::memory_order_relaxed);
    }
    g_count.store(count, std::memory_order_release);
}

int PlaceOf(const void* view) noexcept
{
    const std::int32_t count = g_count.load(std::memory_order_acquire);
    if (count < 0)
    {
        return kUnknown;
    }
    const void* const state = StateOf(view);
    if (state == nullptr)
    {
        return kNotListed;
    }
    for (std::int32_t k = 0; k < count; ++k)
    {
        if (g_states[static_cast<std::size_t>(k)].load(std::memory_order_relaxed) == state)
        {
            return k;
        }
    }
    return kNotListed;
}

}  // namespace lss::render::view_places
