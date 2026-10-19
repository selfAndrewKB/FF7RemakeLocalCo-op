#pragma once

// Taken from Rebirth's hud/overlay_band unchanged in substance (the arithmetic is the engine-independent part;
// Remake's composite uses the same centred cover fit, RE-rdc-split-frame.md §3.4).
//
// Where Square's HUD must be laid out so that a split view shows all of it, and the host scale its
// world markers must use there. Pure arithmetic, tested on the host (tests/overlay_band_test.cpp).
//
// The HUD is one Slate overlay (SEndRootOverlay) rendered into one 16:9 UI image per frame, which
// a post-process pass blends into every view. That pass fits the image to the view as a fixed 16:9
// frame, centred, by cover: scale = max(viewW/imageW, viewH/imageH), so a view of another shape
// shows only a band of the image (RE-p2-hud.md; the fit measured in game, 2026-09-25). Laying the
// overlay out inside that band, instead of the whole image, shows the whole HUD, anchored as a
// native screen of the view's shape would anchor it.
//
// Markers (EndUI__ProjectWorldToOverlay) convert a view point to UI units with
// k = UIScale(layout rect) / hostScale, where UIScale is EndUI__ContainScaleFor1920x1080 of the
// layout rect. A marker lands on its object when k * fitScale = 1, so the host scale must be
// UIScale(band) * fitScale. With the whole image in a solo view this is the engine's own value.
// UIScale itself is the game's function, called by hud/overlay_layout.cpp; the host tests use a
// model of it.

#include <cstdint>

namespace lss::hud {

/// FIntRect as EndUI__GetInGameUIRect fills it: Min then Max, in UI-image pixels.
struct IntRect
{
    std::int32_t minX = 0;
    std::int32_t minY = 0;
    std::int32_t maxX = 0;
    std::int32_t maxY = 0;

    std::int32_t Width() const noexcept { return maxX - minX; }
    std::int32_t Height() const noexcept { return maxY - minY; }
    bool         operator==(const IntRect&) const = default;
};

/// The part of an `imageWidth` x `imageHeight` UI image that a `viewWidth` x `viewHeight` view
/// shows under the centred cover fit. The whole image when the view has the image's shape, and an
/// empty rect for unusable sizes.
IntRect VisibleBand(std::int32_t imageWidth, std::int32_t imageHeight, float viewWidth, float viewHeight) noexcept;

/// The overlay's layout scale with the mod's layout-scale change (hud/layout_scale.h): the contain
/// rule min(height/1080, width/1920), computed as the redirected instructions compute it. For the
/// 16:9 images the game makes this is exactly its own height/1080.
float OverlayLayoutScale(std::int32_t width, std::int32_t height) noexcept;

/// The overlay's layout size in UI units: what widgets placed at absolute layout positions see as
/// the whole screen. A rect laid out at scale s spans rect / s units (1920x1080 for the 16:9 images
/// the game makes; e.g. 3840x1080 for a top/bottom band). Zero for an empty rect.
struct LayoutSize
{
    float width  = 0.0f;
    float height = 0.0f;
    bool  operator==(const LayoutSize&) const = default;
};
inline constexpr LayoutSize kSoloLayout{1920.0f, 1080.0f};
LayoutSize LayoutSizeOf(const IntRect& rect) noexcept;

/// The clip-space map that moves a draw covering the whole target onto `band` of it, unstretched:
/// x' = scaleX * x + translateX * w, y' = scaleY * y + translateY * w (w the clip w).
struct ClipMap
{
    float scaleX     = 1.0f;
    float scaleY     = 1.0f;
    float translateX = 0.0f;
    float translateY = 0.0f;
};
ClipMap BandClipMap(const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight) noexcept;

/// Applies `map` to a row-major FMatrix (clip = v * M; column 3 is clip w), in place.
void ApplyClipMap(float (&matrix)[16], const ClipMap& map) noexcept;

/// The host scale that puts markers on their objects when the overlay is laid out in `band` of the
/// image and the image is cover-fitted to the view: bandUiScale * coverScale. The marker factor is
/// UIScale / host, and `bandUiScale` is the game's own UIScale of the band: its contain scale,
/// EndUI__ContainScaleFor1920x1080 (0x1413A94A0), which the caller calls (the layout scale now
/// equals it). 0 for unusable sizes.
float MarkerHostScale(const IntRect& band, float bandUiScale, std::int32_t imageWidth, std::int32_t imageHeight,
                      float viewWidth, float viewHeight) noexcept;

/// The x/y terms of the overlay entry's projection matrix (FEndMenuLayerEntry, row-major) that
/// draw a layout of `band`'s size unstretched into `band` of an `imageWidth` x `imageHeight`
/// target. The game builds this matrix from the rect's size alone, which stretches any rect over
/// the whole target (FEndMenuLayerEntry__MakeRectMatrix 0x143A07AE0, RE-hud-split.md §1.2); for the whole image
/// these are exactly its terms. The z terms are the game's and are left alone.
struct EntryPlacement
{
    float scaleX     = 0.0f;  // row 0 x
    float scaleY     = 0.0f;  // row 1 y
    float translateX = 0.0f;  // row 3 x
    float translateY = 0.0f;  // row 3 y
};
EntryPlacement PlaceInBand(const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight) noexcept;

}  // namespace lss::hud
