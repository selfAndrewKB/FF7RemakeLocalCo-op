#pragma once

// A render-thread view's family, read with the game's own fields (FSceneView::Family; the family's Views array of
// view pointers and its count), the way the game's stereo rule reads them. A view counts as one only if
// its family lists it: a pointer that is not a live view of the frame yields no family, so a caller can fail open.

#include "engine_facts.h"
#include "platform/guarded.h"

#include <cstdint>

namespace lss::render::view_family {

/// More views than a family of this game ever holds (three players); a larger count is not a family.
inline constexpr std::int32_t kViewsMax = 8;

struct Family
{
    const std::uint8_t* views = nullptr;  // const FSceneView* const*
    std::int32_t        count = 0;        // 0: not a view of a family
    std::int32_t        index = -1;       // the view's place in it
};

/// The view's family object (FSceneView::Family), or null.
inline const void* FamilyOf(const void* view) noexcept
{
    return platform::ReadPointer(view, facts::kViewFamily);
}

/// The family's scene (FSceneViewFamily::Scene), or null.
inline void* SceneOf(const void* family) noexcept
{
    return platform::ReadPointer(family, facts::kFamilyScene);
}

/// The view's family, if the family lists the view; count 0 otherwise. Guarded reads.
inline Family Of(const void* view) noexcept
{
    const void* const family = FamilyOf(view);
    std::int32_t      count  = 0;
    const auto* const views  = static_cast<const std::uint8_t*>(platform::ReadPointer(family, facts::kFamilyViews));
    if (views == nullptr ||
        !platform::Read(static_cast<const std::uint8_t*>(family) + facts::kFamilyViewCount, count) || count < 1 ||
        count > kViewsMax)
    {
        return {};
    }
    for (std::int32_t i = 0; i < count; ++i)
    {
        if (platform::ReadPointer(views, static_cast<std::uint32_t>(i * sizeof(void*))) == view)
        {
            return {views, count, i};
        }
    }
    return {};
}

/// The family's first view; null for no family.
inline const void* First(const Family& family) noexcept
{
    return family.count > 0 ? platform::ReadPointer(family.views, 0) : nullptr;
}

}  // namespace lss::render::view_family
