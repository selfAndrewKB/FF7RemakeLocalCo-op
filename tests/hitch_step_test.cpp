// Host-side tests for the frame watch's hitch rule (platform/hitch_step.h). Run without the game.

#include "platform/hitch_step.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "hitch_step: all passed\n" : "hitch_step: %d failure(s)\n", g_failures);
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

using lss::platform::FrameKind;
using lss::platform::HitchState;
using lss::platform::HitchStep;

constexpr double   kRatio  = 1.6;
constexpr double   kMargin = 5.0;
constexpr unsigned kRun    = 30;
constexpr double   kWeight = 0.05;

FrameKind Step(HitchState& state, double frameMs)
{
    return HitchStep(state, frameMs, kRatio, kMargin, kRun, kWeight);
}

void FirstFrameIsTheAverage()
{
    HitchState state;
    EXPECT(Step(state, 8.3) == FrameKind::First);
    EXPECT(state.averageMs == 8.3);
}

void SteadyFramesMoveTheAverage()
{
    HitchState state;
    Step(state, 10.0);
    EXPECT(Step(state, 12.0) == FrameKind::Steady);
    EXPECT(state.averageMs > 10.0 && state.averageMs < 12.0);
}

void AHitchNeedsBothTheRatioAndTheMargin()
{
    HitchState fast;
    Step(fast, 4.0);
    EXPECT(Step(fast, 8.0) == FrameKind::Steady);  // twice the average, but only 4 ms over it

    HitchState slow;
    Step(slow, 30.0);
    EXPECT(Step(slow, 40.0) == FrameKind::Steady);  // 10 ms over, but under 1.6 times the average

    HitchState both;
    Step(both, 10.0);
    EXPECT(Step(both, 20.0) == FrameKind::Hitch);
    EXPECT(both.averageMs == 10.0);  // a hitch leaves the average alone
}

void ALongRunOfHitchesIsTheNewNormal()
{
    HitchState state;
    Step(state, 10.0);
    for (unsigned i = 0; i < kRun; ++i)
    {
        EXPECT(Step(state, 40.0) == FrameKind::Hitch);
    }
    EXPECT(Step(state, 40.0) == FrameKind::NewNormal);
    EXPECT(state.averageMs == 40.0);
    EXPECT(Step(state, 40.0) == FrameKind::Steady);
}

void ASteadyFrameEndsTheRun()
{
    HitchState state;
    Step(state, 10.0);
    Step(state, 30.0);
    Step(state, 30.0);
    EXPECT(state.run == 2);
    EXPECT(Step(state, 10.0) == FrameKind::Steady);
    EXPECT(state.run == 0);
}

}  // namespace

int main()
{
    FirstFrameIsTheAverage();
    SteadyFramesMoveTheAverage();
    AHitchNeedsBothTheRatioAndTheMargin();
    ALongRunOfHitchesIsTheNewNormal();
    ASteadyFrameEndsTheRun();
    return lss::test::Report();
}
