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

}  // namespace

int main()
{
    Wraps();
    BadReads();
    return lss::test::Report();
}
