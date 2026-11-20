#pragma once

// The composite fits the UI image to a view by a centred cover fit (research/RE-rdc-split-frame.md).
//
// Where Square's HUD must be laid out so that a split view shows all of it, and the host scale its
// world markers must use there. Pure arithmetic, tested on the host (tests/overlay_band_test.cpp).
//
// The HUD is one Slate overlay (SEndRootOverlay) rendered into one 16:9 UI image per frame, which
// a post-process pass blends into every view. That pass fits the image to the view as a fixed 16:9
// frame, centred, by cover: scale = max(viewW/imageW, viewH/imageH), so a view of another shape
// shows only a band of the image (the fit measured in game). Laying the
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
#include <optional>

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

/// The overlay's layout scale with the mod's layout-scale change (hud/layout_code.h): the contain
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
/// EndUI__ContainScaleFor1920x1080, which the caller calls (the layout scale now
/// equals it). 0 for unusable sizes.
float MarkerHostScale(const IntRect& band, float bandUiScale, std::int32_t imageWidth, std::int32_t imageHeight,
                      float viewWidth, float viewHeight) noexcept;

/// The x/y terms of the overlay entry's projection matrix (FEndMenuLayerEntry, row-major) that
/// draw a layout of `band`'s size unstretched into `band` of an `imageWidth` x `imageHeight`
/// target. The game builds this matrix from the rect's size alone, which stretches any rect over
/// the whole target (FEndMenuLayerEntry__MakeRectMatrix, RE-hud-split.md); for the whole image
/// these are exactly its terms. The z terms are the game's and are left alone.
struct EntryPlacement
{
    float scaleX     = 0.0f;  // row 0 x
    float scaleY     = 0.0f;  // row 1 y
    float translateX = 0.0f;  // row 3 x
    float translateY = 0.0f;  // row 3 y
};
EntryPlacement PlaceInBand(const IntRect& band, std::int32_t imageWidth, std::int32_t imageHeight) noexcept;

/// A point in layout units (an FVector2D).
struct LayoutPoint
{
    float x = 0.0f;
    float y = 0.0f;

    bool operator==(const LayoutPoint&) const = default;
};

/// The UI particle draw's viewport in one column's image, as fractions of the image: a box's centre, width and height.
struct EffectBox
{
    float centreX = 0.0f;
    float centreY = 0.0f;
    float width   = 0.0f;
    float height  = 0.0f;

    bool operator==(const EffectBox&) const = default;
};

/// Where the UI particle draw's viewport goes in one column's image so that column's UI particles land where its HUD
/// is (hud/ui_effect_pass.h). The game converts each effect's layout point once, centred in `tickLayout` (the layout
/// the overlay holds at the widget tick that places them: player 1's column's) and in its half-widths, and every
/// column's draw puts that one result over its whole image. The box makes `tickLayout`'s half-width the band's drawing
/// of this column's `layout` units (its width = the band's scale times tickLayout's width) and puts the centre where
/// `tickLayout`'s centre falls in this column's band, moved by the distance the widget the points belong to (anchored
/// at `anchor` of the 1920x1080 frame) stands from its design place in `layout`; with that widget drawn at `size` about
/// its anchored corner (hud/root_placement.h, the HUD size), the box is `size` times as wide and its centre moves with
/// the widget's points. Its height is the width over the
/// column's view's shape (`viewWidth` x `viewHeight`, window pixels): the draw scales the vertical axis by the scene
/// view's aspect, not the viewport's, so a box of the view's shape puts one half-width on both axes, as the game's
/// conversion made them. The image's centre and size for a 16:9 column while `tickLayout` is 16:9 too; past an edge of
/// the image for a wide or tall column, or for a 16:9 one beside a wide `tickLayout`. Nullopt for an unknown layout,
/// band, image or view.
std::optional<EffectBox> EffectViewport(const LayoutSize& tickLayout, const LayoutSize& layout, const IntRect& band,
                                        std::int32_t imageWidth, std::int32_t imageHeight, float viewWidth, float viewHeight,
                                        const LayoutPoint& anchor, float size = 1.0f) noexcept;

}  // namespace lss::hud
