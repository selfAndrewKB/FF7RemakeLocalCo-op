#pragma once

// The pure rule of views/split_scale: how far the split views must be scaled so they fit the scene buffer. Host-tested
// (tests/split_scale_rules_test.cpp).
//
// Square Enix sizes the scene buffer from the game's resolution option (times the screen percentage), never from the
// window; in split the views are laid out in window pixels (the mod switches off the single-view fit, views/
// letterbox_patch). A resolution below the window leaves part of each view outside the buffer, never drawn. The game's
// dynamic resolution scales every view by one fraction without touching the buffer (research/RE-split-upscale.md), so
// capping it at resolution / window keeps every view inside, and the game's final pass scales each back into its rect.
// The screen percentage multiplies both the buffer and the views, so it cancels out. The guarantee holds with one view
// family per player (render/one_family, the default: every view starts at the buffer's corner); with every view in
// the game's one family (`one-family off`, an A/B) a view's origin is rounded up to 4 pixels and can pass the buffer
// by up to 3 rows or columns.

#include <algorithm>

namespace lss::views::split_scale_rules {

inline constexpr int kFull = 100;

/// A view `size` pixels long at `cap` %, sized as the engine sizes it: the fraction in float, the product rounded up
/// (e.g. 3200 x 0.6f is just over 1920, so 1921). If the engine forms the fraction as `Max x 0.01f` rather than
/// `Max / 100` [UNKNOWN], its product is never larger, so the cap still fits (at worst one percent lower than needed).
constexpr int ScaledSize(int size, int cap) noexcept
{
    const float scaled = static_cast<float>(size) * (static_cast<float>(cap) / 100.0f);
    const int   whole  = static_cast<int>(scaled);
    return static_cast<float>(whole) < scaled ? whole + 1 : whole;
}

/// The dynamic-resolution cap, in whole percent (1..100), that keeps views laid out over a `viewX` x `viewY` window
/// inside a `resX` x `resY` buffer: the smaller of resolution / window per axis, floored, then lowered while the engine's
/// float rounding would still take a view a pixel past the buffer. 100 when the resolution covers the window, or when a
/// size is unread (0 or less).
constexpr int CapPercent(int resX, int resY, int viewX, int viewY) noexcept
{
    if (resX <= 0 || resY <= 0 || viewX <= 0 || viewY <= 0)
    {
        return kFull;
    }
    const long long x   = 100LL * resX / viewX;
    const long long y   = 100LL * resY / viewY;
    int             cap = static_cast<int>(std::clamp(std::min(x, y), 1LL, static_cast<long long>(kFull)));
    while (cap > 1 && cap < kFull && (ScaledSize(viewX, cap) > resX || ScaledSize(viewY, cap) > resY))
    {
        --cap;
    }
    return cap;
}

}  // namespace lss::views::split_scale_rules
