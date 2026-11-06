// Host-side tests for the split views' scale rule (views/split_scale_rules.h). Run without the game.

#include "views/split_scale_rules.h"

#include <cmath>
#include <cstdio>

namespace {

using namespace lss::views::split_scale_rules;

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

/// The cap for the cases seen and the edges.
void Caps()
{
    EXPECT(CapPercent(1920, 1080, 2560, 1440) == 75);   // a 1080p option on a 1440p desktop
    EXPECT(CapPercent(2560, 1440, 2560, 1440) == 100);  // the option equal to the window: nothing to do
    EXPECT(CapPercent(3840, 2160, 2560, 1440) == 100);  // above the window: never above 100
    EXPECT(CapPercent(1280, 720, 2560, 1440) == 50);
    EXPECT(CapPercent(2560, 1440, 3840, 2160) == 66);   // 66.67 floored
    EXPECT(CapPercent(1920, 1080, 2560, 1080) == 75);   // a wider window: the tighter axis decides
    EXPECT(CapPercent(1920, 1080, 1920, 1200) == 90);   // a taller one
    EXPECT(CapPercent(7680, 1440, 7680, 1440) == 100);  // the span: the resolution is the row
    EXPECT(CapPercent(0, 1080, 2560, 1440) == 100);     // unread sizes: the game's own cap kept
    EXPECT(CapPercent(1920, 1080, 0, 0) == 100);
    EXPECT(CapPercent(-1, -1, -1, -1) == 100);
    EXPECT(CapPercent(1, 1, 100000, 100000) == 1);      // never below 1
    static_assert(CapPercent(1920, 1080, 2560, 1440) == 75, "constexpr");
    EXPECT(CapPercent(1920, 1080, 4096, 2160) == 46);   // a 4096x2160 TV (read live at 0.46)
    EXPECT(CapPercent(1920, 1080, 3440, 1440) == 55);   // the ultrawide (read live at 0.55)
    EXPECT(CapPercent(1920, 1080, 3200, 1800) == 59);   // 60 exactly, but 3200 x 0.6f rounds up to 1921
    EXPECT(ScaledSize(3200, 60) == 1921);
    EXPECT(ScaledSize(2560, 75) == 1920);
}

/// The floor keeps every view inside the buffer: a view's size times the cap, rounded up as the engine does, never
/// passes the resolution, for the window sizes a player meets.
void FitsTheBuffer()
{
    const int windows[][2] = {{2560, 1440}, {3840, 2160}, {2560, 1080}, {3440, 1440}, {1920, 1200}, {1366, 768}, {7680, 1440},
                              {4096, 2160}, {3200, 1800}, {5120, 1440}, {1600, 900}};
    const int options[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {1366, 768}, {1280, 800}, {1680, 1050}};
    for (const auto& w : windows)
    {
        for (const auto& o : options)
        {
            const int cap = CapPercent(o[0], o[1], w[0], w[1]);
            // The largest view is the whole window (one player's column over a single-player-sized window), sized in
            // float as the engine sizes it.
            const float  f = static_cast<float>(cap) / 100.0f;
            const double x = std::ceil(static_cast<double>(static_cast<float>(w[0]) * f));
            const double y = std::ceil(static_cast<double>(static_cast<float>(w[1]) * f));
            EXPECT(x <= o[0] || cap == 100);
            EXPECT(y <= o[1] || cap == 100);
        }
    }
    // The documented limit: in the game's one family a view's origin is rounded up to 4, so the lower of two stacked
    // views at a 1440x810 option on a 1920x1080 window ends at row 813 of an 812-row buffer.
    const int cap = CapPercent(1440, 810, 1920, 1080);
    EXPECT(cap == 75);
    const int origin = ((540 * cap / 100) + 3) / 4 * 4;  // 405 rounded up to 408
    EXPECT(origin + 540 * cap / 100 == 813);
}

}  // namespace

int main()
{
    Caps();
    FitsTheBuffer();
    std::printf(g_failures == 0 ? "split_scale_rules: all passed\n" : "split_scale_rules: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
