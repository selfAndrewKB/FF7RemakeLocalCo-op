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

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
