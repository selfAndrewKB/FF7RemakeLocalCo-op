// Host-side tests for hud::VisibleBand and MarkerHostScale, run without the game.
//
// The expected values are the ones measured or derived in RE-p2-hud.md: a 3840x2160 UI
// image, a 2560x1440 window, halves of 2560x720 (top/bottom), 1280x1440 (side by side) and 1280x720
// (16:9 with bars). Solo must reproduce the engine's own host scale, 1.333.

#include "hud/overlay_band.h"

#include <cmath>
#include <cstdio>

namespace {

using lss::hud::IntRect;
using lss::hud::MarkerHostScale;
using lss::hud::VisibleBand;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

bool Near(float value, float expected, float tolerance = 0.001f)
{
    return std::fabs(value - expected) <= tolerance;
}

/// A model of the game's EndUI__ContainScaleFor1920x1080, instruction for instruction,
/// for the host only (the mod calls the game's function): 1 for a size within 1e-8 of zero;
/// width/1920 when width/height is below 16/9 or unordered; otherwise height/1080; at least 0.01.
float ContainScaleFor1920x1080(float width, float height)
{
    if (!(std::fabs(width) > 1e-8f) || !(std::fabs(height) > 1e-8f))  // vcomiss + jbe: unordered too
    {
        return 1.0f;
    }
    const float ratio = width / height;  // vcomiss + jb: a NaN ratio takes the width branch
    const float scale = !(ratio >= 1.7777778f) ? width * (1.0f / 1920.0f) : height * (1.0f / 1080.0f);
    return scale > 0.01f ? scale : 0.01f;  // vmaxss: the second operand when the first is not greater
}

/// MarkerHostScale with the band's UI scale from the model, as overlay_layout.cpp passes the game's.
float HostScale(const IntRect& band, std::int32_t imageW, std::int32_t imageH, float viewW, float viewH)
{
    const float ui = ContainScaleFor1920x1080(static_cast<float>(band.Width()), static_cast<float>(band.Height()));
    return MarkerHostScale(band, ui, imageW, imageH, viewW, viewH);
}

void TestBands()
{
    std::puts("band: the part of a 16:9 image each view shape shows under the cover fit");
    EXPECT((VisibleBand(3840, 2160, 2560.0f, 1440.0f) == IntRect{0, 0, 3840, 2160}));   // solo
    EXPECT((VisibleBand(3840, 2160, 1280.0f, 720.0f) == IntRect{0, 0, 3840, 2160}));    // bars
    EXPECT((VisibleBand(3840, 2160, 2560.0f, 720.0f) == IntRect{0, 540, 3840, 1620}));  // top/bottom
    EXPECT((VisibleBand(3840, 2160, 1280.0f, 1440.0f) == IntRect{960, 0, 2880, 2160})); // side by side
    EXPECT((VisibleBand(1920, 1080, 2560.0f, 720.0f) == IntRect{0, 270, 1920, 810}));   // the smaller image
}

void TestHostScales()
{
    std::puts("host scale: solo is the engine's own; each split shape gets the value that keeps markers on objects");
    const auto solo = VisibleBand(3840, 2160, 2560.0f, 1440.0f);
    EXPECT(Near(HostScale(solo, 3840, 2160, 2560.0f, 1440.0f), 1440.0f / 1080.0f));
    const auto bars = VisibleBand(3840, 2160, 1280.0f, 720.0f);
    EXPECT(Near(HostScale(bars, 3840, 2160, 1280.0f, 720.0f), 720.0f / 1080.0f));
    const auto topBottom = VisibleBand(3840, 2160, 2560.0f, 720.0f);
    EXPECT(Near(HostScale(topBottom, 3840, 2160, 2560.0f, 720.0f), 2.0f / 3.0f));
    const auto sideBySide = VisibleBand(3840, 2160, 1280.0f, 1440.0f);  // tall: UI scale = layout scale = 1
    EXPECT(Near(HostScale(sideBySide, 3840, 2160, 1280.0f, 1440.0f), 2.0f / 3.0f));
    // The image size does not change the answer for the same view: 1920x1080 gives the same scale.
    const auto small = VisibleBand(1920, 1080, 2560.0f, 720.0f);
    EXPECT(Near(HostScale(small, 1920, 1080, 2560.0f, 720.0f), 2.0f / 3.0f));
}

void TestMarkerFactorIsOne()
{
    std::puts("host scale: k * fit = 1 in every shape, i.e. a view pixel moves a marker by one view pixel");
    const float views[][2] = {{2560, 1440}, {1280, 720}, {2560, 720}, {1280, 1440}, {3440, 1440}};
    for (const auto& view : views)
    {
        const auto  band = VisibleBand(3840, 2160, view[0], view[1]);
        const float host = HostScale(band, 3840, 2160, view[0], view[1]);
        const float k    = ContainScaleFor1920x1080(static_cast<float>(band.Width()),
                                                              static_cast<float>(band.Height())) / host;
        const float fit  = std::fmax(view[0] / 3840.0f, view[1] / 2160.0f);
        EXPECT(Near(k * fit, 1.0f));
    }
}

void TestPlacement()
{
    std::puts("placement: the whole image gives the game's own matrix terms; a band lands unstretched at its Min");
    using lss::hud::PlaceInBand;
    // The game's matrix for a rect of W x H: (2/W, -2/H) and a constant (-1, 1) translation.
    const auto whole = PlaceInBand(IntRect{0, 0, 3840, 2160}, 3840, 2160);
    EXPECT(Near(whole.scaleX, 2.0f / 3840.0f, 1e-9f) && Near(whole.scaleY, -2.0f / 2160.0f, 1e-9f));
    EXPECT(Near(whole.translateX, -1.0f) && Near(whole.translateY, 1.0f));

    // Top/bottom: rows 540..1620. Layout row 0 lands on NDC y 0.5 (target row 540), row 1080 on -0.5.
    const auto band = PlaceInBand(IntRect{0, 540, 3840, 1620}, 3840, 2160);
    EXPECT(Near(band.scaleX, 2.0f / 3840.0f, 1e-9f) && Near(band.scaleY, -2.0f / 2160.0f, 1e-9f));
    EXPECT(Near(band.translateX, -1.0f) && Near(band.translateY, 0.5f));
    EXPECT(Near(1080.0f * band.scaleY + band.translateY, -0.5f));

    // Side by side: columns 960..2880. Layout column 0 lands on NDC x -0.5, column 1920 on 0.5.
    const auto columns = PlaceInBand(IntRect{960, 0, 2880, 2160}, 3840, 2160);
    EXPECT(Near(columns.translateX, -0.5f) && Near(columns.translateY, 1.0f));
    EXPECT(Near(1920.0f * columns.scaleX + columns.translateX, 0.5f));

    const auto none = PlaceInBand(IntRect{}, 3840, 2160);
    EXPECT(none.scaleX == 0.0f && none.scaleY == 0.0f);
}

void TestContainScaleMatchesTheGame()
{
    std::puts("UI scale: the model of the game's contain scale, and the overlay's scale in bands");
    EXPECT(Near(ContainScaleFor1920x1080(3840.0f, 2160.0f), 2.0f));
    EXPECT(Near(ContainScaleFor1920x1080(1920.0f, 1080.0f), 1.0f));
    EXPECT(Near(ContainScaleFor1920x1080(3840.0f, 1080.0f), 1.0f));   // wider: height
    EXPECT(Near(ContainScaleFor1920x1080(1920.0f, 2160.0f), 1.0f));   // narrower: width
    EXPECT(ContainScaleFor1920x1080(0.0f, 1080.0f) == 1.0f);          // degenerate: 1
    EXPECT(ContainScaleFor1920x1080(5e-9f, 1080.0f) == 1.0f);
    EXPECT(Near(ContainScaleFor1920x1080(1.0f, 1000.0f), 0.01f));     // floor
    EXPECT(ContainScaleFor1920x1080(std::nanf(""), 1080.0f) == 1.0f);  // NaN: unordered against epsilon -> jbe -> 1

    // The layout scale with the contain rule: bit-identical to the game's height/1080 for the 16:9
    // images it makes, and equal to its UI scale for bands of any shape.
    using lss::hud::OverlayLayoutScale;
    EXPECT(OverlayLayoutScale(3840, 2160) == 2160.0f * (1.0f / 1080.0f));
    EXPECT(OverlayLayoutScale(1920, 1080) == 1080.0f * (1.0f / 1080.0f));
    EXPECT(OverlayLayoutScale(3840, 2160) == 2.0f && OverlayLayoutScale(1920, 1080) == 1.0f);
    EXPECT(Near(OverlayLayoutScale(3840, 1080), ContainScaleFor1920x1080(3840.0f, 1080.0f), 1e-6f));  // top/bottom
    EXPECT(Near(OverlayLayoutScale(1920, 2160), ContainScaleFor1920x1080(1920.0f, 2160.0f), 1e-6f));  // side by side
    EXPECT(Near(OverlayLayoutScale(1920, 2160), 1.0f));
}

void TestClipMapMatchesPlacement()
{
    std::puts("clip map: moving the game's whole-rect matrix onto a band gives the placement terms");
    using lss::hud::ApplyClipMap;
    using lss::hud::BandClipMap;
    using lss::hud::PlaceInBand;
    const IntRect bands[] = {{0, 540, 3840, 1620}, {960, 0, 2880, 2160}, {0, 0, 3840, 2160}};
    for (const auto& band : bands)
    {
        // The game's matrix for a rect of the band's size (x/y terms; z column arbitrary, w column 0,0,0,1).
        float m[16] = {2.0f / band.Width(), 0, 0.3f, 0,  0, -2.0f / band.Height(), 0.1f, 0,
                       0, 0, -0.005f, 0,             -1, 1, 0.5f, 1};
        ApplyClipMap(m, BandClipMap(band, 3840, 2160));
        const auto place = PlaceInBand(band, 3840, 2160);
        EXPECT(Near(m[0], place.scaleX, 1e-9f) && Near(m[5], place.scaleY, 1e-9f));
        EXPECT(Near(m[12], place.translateX, 1e-6f) && Near(m[13], place.translateY, 1e-6f));
        EXPECT(m[2] == 0.3f && m[6] == 0.1f && m[10] == -0.005f && m[14] == 0.5f);  // z column untouched
    }
    // A perspective matrix (w from z): the band map uses clip w, so points keep their place in the band.
    float p[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 0, 1,  0, 0, 0.1f, 0};
    ApplyClipMap(p, BandClipMap(IntRect{0, 540, 3840, 1620}, 3840, 2160));
    EXPECT(p[0] == 1.0f && Near(p[5], 0.5f) && p[8] == 0.0f && p[9] == 0.0f);
}

void TestUnusable(){
    std::puts("band and host scale: unusable sizes give empty results");
    EXPECT((VisibleBand(0, 2160, 2560.0f, 720.0f) == IntRect{}));
    EXPECT((VisibleBand(3840, 2160, 0.0f, 720.0f) == IntRect{}));
    EXPECT((VisibleBand(3840, 2160, std::nanf(""), 720.0f) == IntRect{}));
    EXPECT(MarkerHostScale(IntRect{}, 1.0f, 3840, 2160, 2560.0f, 720.0f) == 0.0f);
    EXPECT(MarkerHostScale(IntRect{0, 540, 3840, 1620}, 0.0f, 3840, 2160, 2560.0f, 720.0f) == 0.0f);  // no UI scale
    EXPECT(MarkerHostScale(IntRect{0, 540, 3840, 1620}, std::nanf(""), 3840, 2160, 2560.0f, 720.0f) == 0.0f);
}

void TestLayoutSize()
{
    std::puts("layout size: the whole image is the 1920x1080 frame; a band is larger on its long axis");
    using lss::hud::LayoutSize;
    using lss::hud::LayoutSizeOf;
    EXPECT((LayoutSizeOf(IntRect{0, 0, 3840, 2160}) == LayoutSize{1920.0f, 1080.0f}));    // solo, 4K image
    EXPECT((LayoutSizeOf(IntRect{0, 0, 1920, 1080}) == lss::hud::kSoloLayout));           // solo, 1080p image
    EXPECT((LayoutSizeOf(IntRect{0, 540, 3840, 1620}) == LayoutSize{3840.0f, 1080.0f}));  // top/bottom band
    EXPECT((LayoutSizeOf(IntRect{960, 0, 2880, 2160}) == LayoutSize{1920.0f, 2160.0f}));  // side-by-side band
    EXPECT((LayoutSizeOf(IntRect{}) == LayoutSize{}));
}

void TestEffectViewport()
{
    std::puts("effect viewport: a column's UI particles get a box of the tick layout's width at its band's scale");
    using lss::hud::EffectBox;
    using lss::hud::EffectViewport;
    using lss::hud::LayoutPoint;
    using lss::hud::LayoutSize;
    const LayoutPoint corner{1.0f, 1.0f};
    const auto        near = [](const std::optional<EffectBox>& b, float x, float y, float w, float h) {
        return b && std::fabs(b->centreX - x) < 1e-4f && std::fabs(b->centreY - y) < 1e-4f && std::fabs(b->width - w) < 1e-4f &&
               std::fabs(b->height - h) < 1e-4f;
    };
    const LayoutSize frame{1920.0f, 1080.0f};
    const LayoutSize wide{2580.0f, 1080.0f};
    const IntRect    whole{0, 0, 3840, 2160};
    const IntRect    wideBand{0, 276, 3840, 1884};
    const float      k = 1608.0f / 1080.0f;  // the wide band's pixels per layout unit
    // One 16:9 view: the whole image, as the game draws it.
    EXPECT(near(EffectViewport(frame, frame, whole, 3840, 2160, 1920.0f, 1080.0f, corner), 0.5f, 0.5f, 1.0f, 1.0f));
    // The row's ultrawide (3440x1440) with player 1's 16:9 layout as the tick's: the centre moved right by the 660 units
    // the corner-anchored gauge stands further right than in the frame, at the band's px per unit; the box as wide as
    // the frame at that scale and of the view's shape (its height the width over the view's aspect).
    EXPECT(near(EffectViewport(frame, wide, wideBand, 3840, 2160, 3440.0f, 1440.0f, corner), (960.0f + 660.0f) * k / 3840.0f,
                (276.0f + 540.0f * k) / 2160.0f, 1920.0f * k / 3840.0f, 1920.0f * k * (1440.0f / 3440.0f) / 2160.0f));
    // The same ultrawide as player 1's own layout (the tick's): the box as wide as its layout.
    EXPECT(near(EffectViewport(wide, wide, wideBand, 3840, 2160, 3440.0f, 1440.0f, corner), (1290.0f + 660.0f) * k / 3840.0f,
                (276.0f + 540.0f * k) / 2160.0f, 2580.0f * k / 3840.0f, 2580.0f * k * (1440.0f / 3440.0f) / 2160.0f));
    // A 16:9 column beside it: the gauge at its design place, the tick's centre at 1290 units = 2580 px of 3840, the
    // box 5160 px wide (the one particle, placed for the wide layout, moved past the image's right edge).
    EXPECT(near(EffectViewport(wide, frame, whole, 3840, 2160, 1920.0f, 1080.0f, corner), 2580.0f / 3840.0f, 0.5f, 5160.0f / 3840.0f,
                5160.0f * (1080.0f / 1920.0f) / 2160.0f));
    // Two stacked halves of one shape (3840x1080 units each, one px per unit; views 2560x720): the gauge 1920 units right
    // of its design place, so the centre at the image's right edge; the lower band's middle height; the box as wide as
    // the image and of the half's shape.
    EXPECT(near(EffectViewport(LayoutSize{3840.0f, 1080.0f}, LayoutSize{3840.0f, 1080.0f}, IntRect{0, 1080, 3840, 2160}, 3840, 2160, 2560.0f,
                               720.0f, corner),
                1.0f, 0.75f, 1.0f, 0.5f));
    // Two side-by-side halves (1920x2160 units each, the tick's too; views 1280x1440): the gauge 1080 units below its
    // design place, so the right half's centre at its band's middle width and the image's bottom.
    EXPECT(near(EffectViewport(LayoutSize{1920.0f, 2160.0f}, LayoutSize{1920.0f, 2160.0f}, IntRect{1920, 0, 3840, 2160}, 3840, 2160, 1280.0f,
                               1440.0f, corner),
                0.75f, 1.0f, 0.5f, 1.0f));
    // A centre anchor and a left anchor move the centre by half the extra width and by none of it.
    EXPECT(near(EffectViewport(frame, wide, wideBand, 3840, 2160, 3440.0f, 1440.0f, LayoutPoint{0.5f, 0.0f}), (960.0f + 330.0f) * k / 3840.0f,
                (276.0f + 540.0f * k) / 2160.0f, 1920.0f * k / 3840.0f, 1920.0f * k * (1440.0f / 3440.0f) / 2160.0f));
    EXPECT(near(EffectViewport(frame, wide, wideBand, 3840, 2160, 3440.0f, 1440.0f, LayoutPoint{}), 960.0f * k / 3840.0f,
                (276.0f + 540.0f * k) / 2160.0f, 1920.0f * k / 3840.0f, 1920.0f * k * (1440.0f / 3440.0f) / 2160.0f));
    // With the widget drawn at a HUD size about its corner: the box that many times as wide, its centre moved with the
    // widget's points (the corner stays put). The side-by-side right half at 1.5: the corner at (3840, 2160), the
    // tick's centre 960 units left of it in the design, so 1440 px left at 1.5; the box 2880 px wide.
    EXPECT(near(EffectViewport(LayoutSize{1920.0f, 2160.0f}, LayoutSize{1920.0f, 2160.0f}, IntRect{1920, 0, 3840, 2160}, 3840, 2160, 1280.0f,
                               1440.0f, corner, 1.5f),
                2400.0f / 3840.0f, 1.0f, 2880.0f / 3840.0f, 2880.0f * (1440.0f / 1280.0f) / 2160.0f));
    // The lower stacked half at 0.75: the centre 540 units above the corner in the design, so 405 px above the image's
    // bottom.
    EXPECT(near(EffectViewport(LayoutSize{3840.0f, 1080.0f}, LayoutSize{3840.0f, 1080.0f}, IntRect{0, 1080, 3840, 2160}, 3840, 2160, 2560.0f,
                               720.0f, corner, 0.75f),
                1.0f, 1755.0f / 2160.0f, 0.75f, 2880.0f * (720.0f / 2560.0f) / 2160.0f));
    // Unknowns.
    EXPECT(!EffectViewport(frame, frame, whole, 3840, 2160, 1920.0f, 1080.0f, corner, 0.0f));
    EXPECT(!EffectViewport(frame, frame, whole, 3840, 2160, 1920.0f, 1080.0f, corner, std::nanf("")));
    EXPECT(!EffectViewport(LayoutSize{}, frame, whole, 3840, 2160, 1920.0f, 1080.0f, corner));
    EXPECT(!EffectViewport(frame, LayoutSize{}, whole, 3840, 2160, 1920.0f, 1080.0f, corner));
    EXPECT(!EffectViewport(frame, frame, IntRect{}, 3840, 2160, 1920.0f, 1080.0f, corner));
    EXPECT(!EffectViewport(frame, frame, whole, 0, 2160, 1920.0f, 1080.0f, corner));
    EXPECT(!EffectViewport(frame, frame, whole, 3840, 2160, 0.0f, 1080.0f, corner));
    EXPECT(!EffectViewport(frame, frame, whole, 3840, 2160, 1920.0f, 1080.0f, LayoutPoint{std::nanf(""), 1.0f}));
}
}  // namespace

int main()
{
    TestBands();
    TestHostScales();
    TestMarkerFactorIsOne();
    TestPlacement();
    TestContainScaleMatchesTheGame();
    TestClipMapMatchesPlacement();
    TestUnusable();
    TestLayoutSize();
    TestEffectViewport();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
