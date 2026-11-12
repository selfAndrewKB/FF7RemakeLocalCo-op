#include "hud/overlay_band.h"

#include <algorithm>
#include <cmath>

namespace lss::hud {

namespace {

bool Usable(std::int32_t imageWidth, std::int32_t imageHeight, float viewWidth, float viewHeight) noexcept
{
    return imageWidth > 0 && imageHeight > 0 && std::isfinite(viewWidth) && std::isfinite(viewHeight) &&
           viewWidth >= 1.0f && viewHeight >= 1.0f;
}

float CoverScale(std::int32_t imageWidth, std::int32_t imageHeight, float viewWidth, float viewHeight) noexcept
{
    return std::max(viewWidth / static_cast<float>(imageWidth), viewHeight / static_cast<float>(imageHeight));
}

}  // namespace

IntRect VisibleBand(std::int32_t imageWidth, std::int32_t imageHeight, float viewWidth, float viewHeight) noexcept
{
    if (!Usable(imageWidth, imageHeight, viewWidth, viewHeight))
    {
        return {};
    }
    // The view covers imageSize * scale; the band is the view's own size back in image pixels.
    const float scale = CoverScale(imageWidth, imageHeight, viewWidth, viewHeight);
    const auto  bandW = std::min(imageWidth, static_cast<std::int32_t>(std::lround(viewWidth / scale)));
    const auto  bandH = std::min(imageHeight, static_cast<std::int32_t>(std::lround(viewHeight / scale)));
    const auto  x     = (imageWidth - bandW) / 2;
    const auto  y     = (imageHeight - bandH) / 2;
    return {x, y, x + bandW, y + bandH};
}

namespace {

// The game's constants: 1/1080 (the layout-scale multiply) and 1/1920 (the contain leaf).
constexpr float kInv1920 = 1.0f / 1920.0f;
constexpr float kInv1080 = 1.0f / 1080.0f;

}  // namespace

float OverlayLayoutScale(std::int32_t width, std::int32_t height) noexcept
{
    const float byHeight = static_cast<float>(height) * kInv1080;
    const float byWidth  = static_cast<float>(width) * kInv1920;
    return byHeight < byWidth ? byHeight : byWidth;  // vminss xmm0, xmm0, xmm1: the second when not less
}

LayoutSize LayoutSizeOf(const IntRect& rect) noexcept
{
    const float scale = OverlayLayoutScale(rect.Width(), rect.Height());
    if (rect.Width() <= 0 || rect.Height() <= 0 || !(scale > 0.0f))
    {
        return {};
    }
    return {static_cast<float>(rect.Width()) / scale, static_cast<float>(rect.Height()) / scale};
}

ClipMap BandClipMap(const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight) noexcept
{
    if (imageWidth <= 0 || imageHeight <= 0 || band.Width() <= 0 || band.Height() <= 0)
    {
        return {};
    }
    const float w = static_cast<float>(imageWidth);
    const float h = static_cast<float>(imageHeight);
    const float bw = static_cast<float>(band.Width());
    const float bh = static_cast<float>(band.Height());
    return {bw / w, bh / h, (2.0f * static_cast<float>(band.minX) + bw) / w - 1.0f,
            1.0f - (2.0f * static_cast<float>(band.minY) + bh) / h};
}

void ApplyClipMap(float (&matrix)[16], const ClipMap& map) noexcept
{
    for (int row = 0; row < 4; ++row)
    {
        float* m = matrix + row * 4;
        m[0]     = map.scaleX * m[0] + map.translateX * m[3];
        m[1]     = map.scaleY * m[1] + map.translateY * m[3];
    }
}

float MarkerHostScale(const IntRect& band, float bandUiScale, std::int32_t imageWidth, std::int32_t imageHeight,
                      float viewWidth, float viewHeight) noexcept
{
    if (!Usable(imageWidth, imageHeight, viewWidth, viewHeight) || band.Width() <= 0 || band.Height() <= 0 ||
        !std::isfinite(bandUiScale) || bandUiScale <= 0.0f)
    {
        return 0.0f;
    }
    return bandUiScale * CoverScale(imageWidth, imageHeight, viewWidth, viewHeight);
}

EntryPlacement PlaceInBand(const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight) noexcept
{
    if (imageWidth <= 0 || imageHeight <= 0 || band.Width() <= 0 || band.Height() <= 0)
    {
        return {};
    }
    // A layout pixel p lands on target pixel band.Min + p:
    //   NDC.x = 2 (Min.x + p.x) / W - 1,   NDC.y = 1 - 2 (Min.y + p.y) / H.
    const float w = static_cast<float>(imageWidth);
    const float h = static_cast<float>(imageHeight);
    return {2.0f / w, -2.0f / h, 2.0f * static_cast<float>(band.minX) / w - 1.0f,
            1.0f - 2.0f * static_cast<float>(band.minY) / h};
}

std::optional<EffectBox> EffectViewport(const LayoutSize& tickLayout, const LayoutSize& layout, const IntRect& band,
                                        std::int32_t imageWidth, std::int32_t imageHeight, const LayoutPoint& anchor) noexcept
{
    if (!(tickLayout.width > 0.0f) || !(tickLayout.height > 0.0f) || !(layout.width > 0.0f) || !(layout.height > 0.0f) ||
        band.Width() <= 0 || band.Height() <= 0 || imageWidth <= 0 || imageHeight <= 0 || !std::isfinite(anchor.x) ||
        !std::isfinite(anchor.y))
    {
        return std::nullopt;
    }
    // The band's drawing: layout units onto the band's pixels. The game's result is in half-widths of `tickLayout`, and
    // the draw puts one half-width at half the box, so the box is tickLayout's width in the band's pixels per unit. The
    // game converts the design point against `tickLayout`'s centre, so a point at that centre lands at the box's centre;
    // in this column the widget the points belong to stands anchor * (layout - frame) from its design place, so the
    // centre goes there too.
    const float scale   = static_cast<float>(band.Height()) / layout.height;
    const float centreX = static_cast<float>(band.minX) + (tickLayout.width / 2.0f + anchor.x * (layout.width - 1920.0f)) * scale;
    const float centreY = static_cast<float>(band.minY) + (tickLayout.height / 2.0f + anchor.y * (layout.height - 1080.0f)) * scale;
    const float width   = tickLayout.width * scale;
    return EffectBox{centreX / static_cast<float>(imageWidth), centreY / static_cast<float>(imageHeight),
                     width / static_cast<float>(imageWidth)};
}

}  // namespace lss::hud
