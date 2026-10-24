#pragma once

// One view family per player: the loop that hands a family of several views to the renderer one view at a time
// (RE-haze-occlusion.md Pass 9 H.3; ported from Rebirth's renderer/one_family_per_view, without its +0x74 write).
//
// Why: a view after the first in a family loses its volumetric fog after a late join (the blue haze), and the haze
// follows the place in the family, not the player [CONFIRMED live 2026-10-24 07:20, the view-order swap]. As the first
// view of its own family every player is rendered the way the game renders a single player.
//
// How: for view k, view k is swapped into slot 0 and the count set to 1; the families after the first set Square
// Enix's frame-number skip (+0x70), so the scene's frame number steps once a frame. The renderer copies the family and
// its views when a submission starts (0x1425C6A20), so one family object serves every submission. The views are
// swapped inside their own array, and the array, count and flag are put back exactly before returning: Draw deletes
// its views through this list afterwards.
//
// Pure logic over the family's memory (tests/family_split_test.cpp).

#include "engine_facts.h"

#include <cstdint>
#include <cstring>
#include <utility>

namespace lss::render::family_split {

/// More views than a family of this game holds (three players); a larger count is left whole.
inline constexpr std::int32_t kViewsMax = 8;

/// Submits `family` once per view (`submit(family)`), each time holding only that view, in the family's order.
/// `before(k, n, view)` runs before view k's submission, k = 0..n-1, only when the family is split; false stops the
/// split: at view 0 the family goes through one ordinary `submit` instead, at a later view the frame's remaining views
/// are not drawn (the families before them are; a later family without its boundary reset could read freed state). A
/// family of one view (or a count outside 2..kViewsMax, or no array) goes through one ordinary `submit`. Returns the
/// submissions made.
template <class Submit, class Before>
int SubmitOnePerView(std::uint8_t* family, Submit&& submit, Before&& before)
{
    const void** views = nullptr;
    std::memcpy(&views, family + facts::kFamilyViews, sizeof(views));
    std::int32_t count = 0;
    std::memcpy(&count, family + facts::kFamilyViewCount, sizeof(count));
    if (views == nullptr || count < 2 || count > kViewsMax)
    {
        submit(family);
        return 1;
    }

    // A dword: the submit's gate reads it whole (`cmp dword [family+70h], 0`, Pass 10 listing of 0x1425C9770).
    std::int32_t skip = 0;
    std::memcpy(&skip, family + facts::kFamilySkipFrameStep, sizeof(skip));
    const std::int32_t one = 1;
    for (std::int32_t k = 0; k < count; ++k)
    {
        if (!before(k, count, views[k]))
        {
            if (k == 0)
            {
                submit(family);
                return 1;
            }
            return k;
        }
        std::swap(views[0], views[k]);  // view k into slot 0; nothing for k == 0
        std::memcpy(family + facts::kFamilyViewCount, &one, sizeof(one));
        if (k > 0)
        {
            std::memcpy(family + facts::kFamilySkipFrameStep, &one, sizeof(one));
        }

        submit(family);

        // Put this view back before the next one comes forward: at every step the array is the game's apart from
        // one swap.
        std::swap(views[0], views[k]);
        std::memcpy(family + facts::kFamilyViewCount, &count, sizeof(count));
        std::memcpy(family + facts::kFamilySkipFrameStep, &skip, sizeof(skip));
    }
    return count;
}

}  // namespace lss::render::family_split
