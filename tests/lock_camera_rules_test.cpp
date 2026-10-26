// Host-side tests for the lock camera readings' arithmetic (battle/lock_camera_rules.h). Run without the game.

#include "battle/lock_camera_rules.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "lock_camera_rules: all passed\n" : "lock_camera_rules: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
}  // namespace lss::test

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++lss::test::g_failures;                                         \
        }                                                                    \
    } while (false)

namespace {

using lss::battle::lock_camera_rules::BurstClosed;
using lss::battle::lock_camera_rules::IsJump;
using lss::battle::lock_camera_rules::kQuietFrames;
using lss::battle::lock_camera_rules::WrapDegrees;

bool Near(float a, float b)
{
    return std::fabs(a - b) < 0.001f;
}

/// A yaw difference lands in [-180, 180] whichever way round it came.
void Wraps()
{
    EXPECT(Near(WrapDegrees(190.0f), -170.0f));
    EXPECT(Near(WrapDegrees(-190.0f), 170.0f));
    EXPECT(Near(WrapDegrees(540.0f), 180.0f) || Near(WrapDegrees(540.0f), -180.0f));
    EXPECT(Near(std::fabs(WrapDegrees(180.0f)), 180.0f));
    EXPECT(Near(std::fabs(WrapDegrees(-180.0f)), 180.0f));
    EXPECT(Near(WrapDegrees(0.0f), 0.0f));
    EXPECT(Near(WrapDegrees(45.5f), 45.5f));
    EXPECT(Near(WrapDegrees(-45.5f), -45.5f));
}

/// A raw read gone wrong (an infinity, a NaN, a huge value) comes back without looping.
void BadReads()
{
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT(std::isinf(WrapDegrees(inf)));
    EXPECT(std::isinf(WrapDegrees(-inf)));
    EXPECT(std::isnan(WrapDegrees(std::numeric_limits<float>::quiet_NaN())));
    const float huge = WrapDegrees(1.5e10f);
    EXPECT(std::isfinite(huge) && std::fabs(huge) <= 180.0f);
}

/// A jump is a big step taken fast; a stick turn, a follow, a hitch's long tick are not.
void Jumps()
{
    constexpr float frame = 1.0f / 120.0f;
    EXPECT(IsJump(30.0f, 0.0f, frame));      // a reset: 30 degrees in one frame
    EXPECT(IsJump(0.0f, 400.0f, frame));     // a cut: 4 m in one frame
    EXPECT(!IsJump(3.0f, 5.0f, frame));      // a stick turn and a follow
    EXPECT(!IsJump(9.9f, 0.0f, frame));      // under the step size
    EXPECT(!IsJump(12.0f, 0.0f, 0.1f));      // a hitch: 12 degrees over 100 ms is 120 degrees a second
    EXPECT(!IsJump(0.0f, 150.0f, 0.1f));     // a hitch: 1.5 m over 100 ms is 15 m/s
    EXPECT(IsJump(12.0f, 0.0f, 0.0f));       // no tick time: the step decides
    EXPECT(IsJump(0.0f, 150.0f, -1.0f));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT(!IsJump(nan, nan, frame));        // bad reads
    EXPECT(!IsJump(inf, 0.0f, frame));
    EXPECT(IsJump(30.0f, 0.0f, nan));        // a bad tick time: the step decides
}

/// A burst stays open through short quiet spells (a jump every few ticks) and closes after a quiet fifth of a second.
void Bursts()
{
    EXPECT(!BurstClosed(1000, 1000));
    EXPECT(!BurstClosed(1000, 1002));                    // jump, quiet, jump: one burst
    EXPECT(!BurstClosed(1000, 1010));
    EXPECT(!BurstClosed(1000, 1000 + kQuietFrames));
    EXPECT(BurstClosed(1000, 1001 + kQuietFrames));
    EXPECT(BurstClosed(1000, 999));                      // the counter went back
}

}  // namespace

int main()
{
    Wraps();
    BadReads();
    Jumps();
    Bursts();
    return lss::test::Report();
}
