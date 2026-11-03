// Host-side tests for the split FOV rule (views/split_fov_rules.h). Run without the game.

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

/// Which views take the rule: the user's two cases, and every view that must keep the engine's FOV.
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
    EXPECT(Applies({3440, 1440, 8560, 1440}, kSolo));   // the ultrawide of a 2560 | 3440 | 2560 span (v0.147.0)
    EXPECT(!Applies({2560, 1440, 8560, 1440}, kSolo));  // a 16:9 monitor of that span
    EXPECT(Applies({3440, 1440, 6000, 1440}, kSolo));   // the ultrawide of a two-monitor span
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

}  // namespace

int main()
{
    WhoTakesIt();
    Values();
    if (g_failures != 0)
    {
        std::printf("split_fov_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("split_fov_rules: all passed\n");
    return 0;
}
