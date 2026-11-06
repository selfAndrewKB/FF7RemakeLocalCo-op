// Host-side tests for the split FOV rule (views/split_fov_rules.h). Run without the game.

#include "platform/settings.h"
#include "views/split_fov_rules.h"

#include <cmath>
#include <cstdio>

namespace {

using namespace lss::views::split_fov_rules;

int g_failures = 0;

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

constexpr float kSolo = 16.0f / 9.0f;

bool Near(float a, float b, float slack = 0.05f)
{
    return std::fabs(a - b) <= slack;
}

float TanHalf(float degrees)
{
    return std::tan(degrees * 3.14159265358979f / 360.0f);
}

/// Which views take the rule: the two cases it is for, and every view that must keep the engine's FOV.
void WhoTakesIt()
{
    EXPECT(Applies({1920, 540, 1920, 1080}, kSolo));    // two players in one window, each half
    EXPECT(Applies({2560, 720, 2560, 1440}, kSolo));    // the same at 1440p
    EXPECT(Applies({1920, 540, 1920, 1080}, kSolo));    // three players: the top view (FavorTop)
    EXPECT(!Applies({960, 540, 1920, 1080}, kSolo));    // three players: a bottom quarter is 16:9
    EXPECT(!Applies({1920, 1080, 1920, 1080}, kSolo));  // the single view
    EXPECT(!Applies({1920, 1080, 5760, 1080}, kSolo));  // one monitor of a row of three
    EXPECT(!Applies({640, 1080, 1920, 1080}, kSolo));   // a full-height column (view-columns 3)
    EXPECT(!Applies({960, 1080, 1920, 1080}, kSolo));   // side by side
    EXPECT(!Applies({2560, 1080, 2560, 1080}, kSolo));  // a 21:9 single view: as tall as the single-player picture
    EXPECT(Applies({2560, 540, 2560, 1080}, kSolo));    // two players in a 21:9 window
    EXPECT(Applies({3440, 1440, 8560, 1440}, kSolo));   // the ultrawide of a 2560 | 3440 | 2560 span
    EXPECT(!Applies({2560, 1440, 8560, 1440}, kSolo));  // a 16:9 monitor of that span
    EXPECT(Applies({3440, 1440, 6000, 1440}, kSolo));   // the ultrawide of a two-monitor span
    // A monitor shorter than the row's tallest is judged on its own height: the wide one keeps the camera's vertical FOV like any
    // other wide column, and a 16:9 one stays with the engine, though each is shorter than the window.
    using lss::views::split_fov_rules::HeightRatio;
    using lss::views::split_fov_rules::OnItsMonitor;
    using lss::views::split_fov_rules::SplitFov;
    const auto shortWide = OnItsMonitor({2560, 1080, 5120, 1440});
    EXPECT(Applies(shortWide, kSolo) && HeightRatio(shortWide, kSolo) == 1.0f);
    EXPECT(SplitFov(90.0f, kSolo, shortWide, 0.5f) == SplitFov(90.0f, kSolo, shortWide, 1.0f));  // no zoom: the weight has nothing to act on
    EXPECT(!Applies(OnItsMonitor({1280, 720, 3840, 1440}), kSolo));
    EXPECT(!Applies(OnItsMonitor({2560, 1440, 5120, 1440}), kSolo));
    EXPECT(!Applies({0, 540, 1920, 1080}, kSolo));      // unread sizes
    EXPECT(!Applies({1920, 540, 1920, 1080}, 0.0f));
}

/// The rule's values: Rebirth's formula, measured against the single-player picture.
void Values()
{
    const float fov = 90.0f;  // horizontal at 16:9
    // A half (height 0.5): tan(V/2) = tan(Vs/2) * sqrt(0.5); the horizontal tangent is sqrt(2) times the camera's.
    const float half = SplitFov(fov, kSolo, {1920, 540, 1920, 1080}, kDefaultWeight);
    EXPECT(Near(TanHalf(half) / TanHalf(fov), std::sqrt(2.0f), 0.001f));
    // Weight 0 keeps the single-player vertical (the width doubles); weight 1 keeps the scale (the engine's own FOV).
    EXPECT(Near(TanHalf(SplitFov(fov, kSolo, {1920, 540, 1920, 1080}, 0.0f)) / TanHalf(fov), 2.0f, 0.001f));
    EXPECT(Near(SplitFov(fov, kSolo, {1920, 540, 1920, 1080}, 1.0f), fov, 0.01f));
    // The window's size does not matter, only the shapes.
    EXPECT(Near(SplitFov(fov, kSolo, {2560, 720, 2560, 1440}, kDefaultWeight), half, 0.01f));
    // A 21:9 window: the single-player picture is the window's height, so the half is 0.5 of it, at its wider shape.
    const float wide = SplitFov(fov, kSolo, {2560, 540, 2560, 1080}, kDefaultWeight);
    const float tanVertical = TanHalf(fov) / kSolo * std::sqrt(0.5f);
    EXPECT(Near(TanHalf(wide), tanVertical * 2560.0f / 540.0f, 0.001f));
    // The ultrawide of a span keeps the camera's vertical FOV (Hor+) at any weight: the horizontal tangent grows by the
    // view's aspect over the camera's.
    const float columnTan = TanHalf(fov) * (3440.0f / 1440.0f) / kSolo;
    EXPECT(Near(TanHalf(SplitFov(fov, kSolo, {3440, 1440, 8560, 1440}, kDefaultWeight)), columnTan, 0.001f));
    EXPECT(Near(TanHalf(SplitFov(fov, kSolo, {3440, 1440, 8560, 1440}, 1.0f)), columnTan, 0.001f));
    EXPECT(SplitFov(fov, kSolo, {2560, 1440, 8560, 1440}, kDefaultWeight) == fov);
    // Views the rule leaves alone, and unusable inputs, come back unchanged.
    EXPECT(SplitFov(fov, kSolo, {960, 540, 1920, 1080}, kDefaultWeight) == fov);
    EXPECT(SplitFov(fov, kSolo, {1920, 540, 1920, 1080}, 1.5f) == fov);
    EXPECT(SplitFov(fov, kSolo, {1920, 540, 1920, 1080}, -0.1f) == fov);
    EXPECT(SplitFov(0.5f, kSolo, {1920, 540, 1920, 1080}, kDefaultWeight) == 0.5f);
    EXPECT(std::isnan(SplitFov(NAN, kSolo, {1920, 540, 1920, 1080}, kDefaultWeight)));  // returned as given
    // A narrow camera FOV stays inside the valid range.
    const float narrow = SplitFov(20.0f, kSolo, {1920, 540, 1920, 1080}, kDefaultWeight);
    EXPECT(narrow > 20.0f && narrow < 40.0f);
    EXPECT(Near(HeightRatio({1920, 540, 1920, 1080}, kSolo), 0.5f, 0.0001f));
    EXPECT(Near(HeightRatio({1920, 540, 1920, 1200}, kSolo), 0.5f, 0.0001f));  // 16:10: the picture is 1080 tall
}

/// Which views take the narrow rule, and its values: Rebirth's measured table at 8:9 from a 60 degree camera.
void Narrow()
{
    EXPECT(AppliesNarrow({960, 1080, 1920, 1080}, kSolo));    // side by side
    EXPECT(AppliesNarrow({1280, 1440, 2560, 1440}, kSolo));   // the same at 1440p
    EXPECT(AppliesNarrow({1720, 1440, 3440, 1440}, kSolo));   // side by side on an ultrawide
    EXPECT(AppliesNarrow({640, 1080, 1920, 1080}, kSolo));    // a column of view-columns 3
    EXPECT(AppliesNarrow({800, 1200, 1600, 1200}, kSolo));    // side by side in a 4:3 window: taller than the picture
    EXPECT(!AppliesNarrow({800, 600, 1600, 1200}, kSolo));    // a quarter of that window: shorter than the picture
    EXPECT(!AppliesNarrow({1600, 1200, 1600, 1200}, kSolo));  // a 4:3 single view: as wide as the picture
    EXPECT(!AppliesNarrow({1920, 1200, 1920, 1200}, kSolo));  // a 16:10 single view
    EXPECT(!AppliesNarrow({1920, 1080, 1920, 1080}, kSolo));  // the single view
    EXPECT(!AppliesNarrow({1920, 1080, 5760, 1080}, kSolo));  // one monitor of a row
    EXPECT(!AppliesNarrow({960, 540, 1920, 1080}, kSolo));    // a 16:9 quarter
    EXPECT(!AppliesNarrow({1920, 540, 1920, 1080}, kSolo));   // top and bottom
    EXPECT(!AppliesNarrow({3440, 1440, 8560, 1440}, kSolo));  // a span's ultrawide
    EXPECT(!AppliesNarrow({0, 1080, 1920, 1080}, kSolo));     // unread sizes
    EXPECT(!AppliesNarrow({960, 1080, 1920, 1080}, 0.0f));
    // No view takes both rules.
    EXPECT(!Applies({960, 1080, 1920, 1080}, kSolo));

    const Shape half{1280, 1440, 2560, 1440};
    EXPECT(Near(NarrowFov(60.0f, kSolo, half, 0.0f), 60.0f));
    EXPECT(Near(NarrowFov(60.0f, kSolo, half, 0.35f), 48.7f));
    EXPECT(Near(NarrowFov(60.0f, kSolo, half, 0.5f), 44.4f));
    EXPECT(Near(NarrowFov(60.0f, kSolo, half, 0.65f), 40.4f));
    EXPECT(Near(NarrowFov(60.0f, kSolo, half, 1.0f), 32.2f));
    // Weight 1 keeps the single-player vertical FOV: the vertical tangent is the camera's.
    EXPECT(Near(TanHalf(NarrowFov(60.0f, kSolo, half, 1.0f)) / (8.0f / 9.0f), TanHalf(60.0f) / kSolo, 0.001f));
    // An ultrawide's half is measured against the 16:9 picture, not the window: the same scale as single player.
    const Shape ultrawide{1720, 1440, 3440, 1440};
    EXPECT(Near(TanHalf(NarrowFov(60.0f, kSolo, ultrawide, 1.0f)) / TanHalf(60.0f), 1720.0f / 2560.0f, 0.001f));
    // Views the rule leaves alone, and unusable inputs, come back unchanged.
    EXPECT(NarrowFov(60.0f, kSolo, {1920, 540, 1920, 1080}, 1.0f) == 60.0f);
    EXPECT(NarrowFov(60.0f, kSolo, half, 1.5f) == 60.0f);
    EXPECT(NarrowFov(60.0f, kSolo, half, -0.1f) == 60.0f);
    EXPECT(NarrowFov(0.5f, kSolo, half, 1.0f) == 0.5f);
    EXPECT(std::isnan(NarrowFov(NAN, kSolo, half, 1.0f)));
}

/// The sideways offset follows the view's width, only ever softened, no further than the minimum.
void Shoulder()
{
    const Shape half{1280, 1440, 2560, 1440};
    EXPECT(Near(ShoulderFactor(half, 1.0f), kShoulderMinimum, 0.0001f));   // 0.5 kept at the minimum
    EXPECT(Near(ShoulderFactor(half, 0.5f), std::sqrt(0.5f), 0.0001f));    // 0.71, above it
    EXPECT(ShoulderFactor(half, 0.0f) == 1.0f);                            // the whole width: the game's own offset
    EXPECT(Near(ShoulderFactor({1720, 1440, 3440, 1440}, 1.0f), 1720.0f / 2560.0f < kShoulderMinimum ? kShoulderMinimum : 1720.0f / 2560.0f, 0.0001f));
    EXPECT(ShoulderFactor({1920, 540, 1920, 1080}, 1.0f) == 1.0f);         // top and bottom: the game's own
    EXPECT(ShoulderFactor({1920, 1080, 1920, 1080}, 1.0f) == 1.0f);        // the single view
    EXPECT(ShoulderFactor({960, 540, 1920, 1080}, 1.0f) == 1.0f);          // a quarter
    EXPECT(ShoulderFactor(half, 1.5f) == 1.0f);                            // an unusable weight
}

}  // namespace

/// The settings' words read as the weights they say, in order; the option's default is the rule's.
void Weights()
{
    namespace settings = lss::platform::settings;
    EXPECT(WeightOf(settings::kHorizontalFov.fallback) == kDefaultWeight);
    EXPECT(settings::kHorizontalFov.words.size() == 21);
    float last = -1.0f;
    for (const wchar_t* word : settings::kHorizontalFov.words)
    {
        const float weight = WeightOf(word);
        EXPECT(weight > last);
        last = weight;
    }
    EXPECT(WeightOf(L"0.00") == 0.0f);
    EXPECT(WeightOf(L"1.00") == 1.0f);
    EXPECT(Near(WeightOf(L"0.35"), 0.35f, 0.0001f));
    EXPECT(WeightOf(L"") == kDefaultWeight);
    EXPECT(WeightOf(L"1.05") == kDefaultWeight);
    EXPECT(WeightOf(L"-0.5") == kDefaultWeight);
    EXPECT(WeightOf(L"0.5x") == kDefaultWeight);
    EXPECT(WeightOf(L"0,35") == kDefaultWeight);
    EXPECT(WeightOf(settings::kVerticalFov.fallback, kDefaultNarrowWeight) == kDefaultNarrowWeight);
    EXPECT(WeightOf(L"nonsense", kDefaultNarrowWeight) == kDefaultNarrowWeight);
    EXPECT(WeightOf(L"0.35", kDefaultNarrowWeight) != kDefaultNarrowWeight);
}

int main()
{
    WhoTakesIt();
    Values();
    Narrow();
    Shoulder();
    Weights();
    if (g_failures != 0)
    {
        std::printf("split_fov_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("split_fov_rules: all passed\n");
    return 0;
}
