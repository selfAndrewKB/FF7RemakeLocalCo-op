// Host-side tests for the frame capture's start and end rule (render/capture_rules.h). Run without the game.

#include "render/capture_rules.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "capture_rules: all passed\n" : "capture_rules: %d failure(s)\n", g_failures);
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

using namespace lss::render::capture_rules;

constexpr double kFrame = 1000.0 / 120.0;

/// `ms` of frames of `frameMs`; the last step's answer, and how many frames gave `counted`.
struct Run
{
    Step last    = Step::Idle;
    int  counted = 0;
};
Run Frames(State& s, const Config& c, double ms, double frameMs, bool action, bool battle, Step counted = Step::Start)
{
    Run run;
    for (double t = 0.0; t < ms; t += frameMs)
    {
        run.last = CaptureStep(s, c, Frame{.frameMs = frameMs, .action = action, .battle = battle});
        run.counted += run.last == counted ? 1 : 0;
    }
    return run;
}

State Settled(const Config& c)
{
    State s;
    Frames(s, c, 20000.0, kFrame, false, true);
    return s;
}

void AnActionStartsOnItsFirstFrame()
{
    const Config c;
    State        s = Settled(c);
    EXPECT(CaptureStep(s, c, Frame{.frameMs = kFrame, .action = true}) == Step::Start);
    EXPECT(s.reason == Reason::Action && s.phase == Phase::During);
}

void AnActionCaptureKeepsTheFramesAfterAndEnds()
{
    const Config c;
    State        s = Settled(c);
    EXPECT(Frames(s, c, 4500.0, kFrame, true, true).counted == 1);
    EXPECT(s.phase == Phase::During);
    const Run after = Frames(s, c, c.afterMs - 100.0, kFrame, false, true, Step::End);
    EXPECT(after.counted == 0 && s.phase == Phase::After);
    EXPECT(Frames(s, c, 200.0, kFrame, false, true, Step::End).counted == 1);
    EXPECT(s.phase == Phase::Idle);
}

void AnActionAgainInTheFramesAfterContinuesTheCapture()
{
    const Config c;
    State        s = Settled(c);
    Frames(s, c, 1000.0, kFrame, true, true);
    Frames(s, c, 1000.0, kFrame, false, true);
    EXPECT(s.phase == Phase::After);
    EXPECT(Frames(s, c, 1000.0, kFrame, true, true, Step::Start).counted == 0);
    EXPECT(s.phase == Phase::During);
}

void ADropInBattleStartsAfterItsHold()
{
    const Config c;
    State        s = Settled(c);
    const double slow = 1000.0 / 80.0;  // 120 fps to 80
    const Run    short_ = Frames(s, c, 300.0, slow, false, true);
    EXPECT(short_.counted == 0);
    EXPECT(Frames(s, c, 1500.0, slow, false, true).counted == 1);
    EXPECT(s.reason == Reason::Drop && s.phase == Phase::During);
}

void ADropOutsideBattleStartsNothing()
{
    const Config c;
    State        s = Settled(c);
    EXPECT(Frames(s, c, 5000.0, 1000.0 / 60.0, false, false).counted == 0);
    EXPECT(s.phase == Phase::Idle);
}

void TheDropTriggerCanBeOff()
{
    const Config c;
    State        s = Settled(c);
    int          starts = 0;
    for (int i = 0; i < 600; ++i)
    {
        starts += CaptureStep(s, c, Frame{.frameMs = 1000.0 / 60.0, .battle = true, .drops = false}) == Step::Start ? 1 : 0;
    }
    EXPECT(starts == 0);
}

void ASingleHitchStartsNothingAndBarelyMovesTheBaseline()
{
    const Config c;
    State        s      = Settled(c);
    const double before = s.baselineMs;
    EXPECT(CaptureStep(s, c, Frame{.frameMs = 250.0, .battle = true}) == Step::Idle);
    EXPECT(Frames(s, c, 2000.0, kFrame, false, true).counted == 0);
    EXPECT(s.baselineMs - before < 0.5);
}

void AStallOfASecondStartsNothing()
{
    const Config c;
    State        s = Settled(c);
    EXPECT(CaptureStep(s, c, Frame{.frameMs = 1000.0, .battle = true}) == Step::Idle);
    EXPECT(Frames(s, c, 2000.0, kFrame, false, true).counted == 0);
}

void AFewHitchesInARowStartNothing()
{
    const Config c;
    State        s      = Settled(c);
    int          starts = 0;
    for (int i = 0; i < 3; ++i)
    {
        starts += CaptureStep(s, c, Frame{.frameMs = 100.0, .battle = true}) == Step::Start ? 1 : 0;
        starts += Frames(s, c, 50.0, kFrame, false, true).counted;
    }
    starts += Frames(s, c, 2000.0, kFrame, false, true).counted;
    EXPECT(starts == 0);
}

void ADropCaptureEndsAfterItAndDoesNotStartAgain()
{
    const Config c;
    State        s = Settled(c);
    EXPECT(Frames(s, c, 3000.0, 1000.0 / 60.0, false, true).counted == 1);
    EXPECT(Frames(s, c, c.afterMs + 1000.0, kFrame, false, true, Step::End).counted == 1);
    EXPECT(s.phase == Phase::Idle);
    EXPECT(Frames(s, c, 5000.0, kFrame, false, true).counted == 0);
}

void TheLongestCaptureEndsInTheFramesAfterToo()
{
    const Config c{.afterMs = 3000.0, .longestMs = 2000.0};
    State        s = Settled(c);
    Frames(s, c, 1000.0, kFrame, true, true);
    EXPECT(Frames(s, c, 1500.0, kFrame, false, true, Step::End).counted == 1);
    EXPECT(s.phase == Phase::Idle);
}

void AnEmptyFrameChangesNothing()
{
    const Config c;
    State        s = Settled(c);
    EXPECT(CaptureStep(s, c, Frame{.frameMs = 0.0, .action = true}) == Step::Idle);
    EXPECT(CaptureStep(s, c, Frame{.frameMs = kFrame, .action = true}) == Step::Start);
    const double length = s.lengthMs;
    EXPECT(CaptureStep(s, c, Frame{.frameMs = 0.0}) == Step::Capture);
    EXPECT(s.lengthMs == length && s.phase == Phase::During);
}

void ASmallDropStartsNothing()
{
    const Config c;
    State        s = Settled(c);
    EXPECT(Frames(s, c, 5000.0, 1000.0 / 110.0, false, true).counted == 0);  // 9.1 ms against 8.3: under both bars
}

void TheLongestCaptureEndsAndADropStillHoldingIsTheNewNormal()
{
    const Config c{.longestMs = 10000.0};
    State        s    = Settled(c);
    const double slow = 1000.0 / 60.0;
    EXPECT(Frames(s, c, 600.0 + c.longestMs, slow, false, true, Step::End).counted == 1);
    EXPECT(s.phase == Phase::Idle);
    EXPECT(s.baselineMs > 15.0);
    EXPECT(Frames(s, c, 5000.0, slow, false, true).counted == 0);
}

void TheFirstFrameSetsTheBaseline()
{
    const Config c;
    State        s;
    EXPECT(CaptureStep(s, c, Frame{.frameMs = 12.0, .action = true}) == Step::Idle);
    EXPECT(s.baselineMs == 12.0);
    EXPECT(CaptureStep(s, c, Frame{.frameMs = 12.0, .action = true}) == Step::Start);
}

}  // namespace

int main()
{
    AnActionStartsOnItsFirstFrame();
    AnActionCaptureKeepsTheFramesAfterAndEnds();
    AnActionAgainInTheFramesAfterContinuesTheCapture();
    ADropInBattleStartsAfterItsHold();
    ADropOutsideBattleStartsNothing();
    TheDropTriggerCanBeOff();
    ASingleHitchStartsNothingAndBarelyMovesTheBaseline();
    AStallOfASecondStartsNothing();
    AFewHitchesInARowStartNothing();
    ADropCaptureEndsAfterItAndDoesNotStartAgain();
    TheLongestCaptureEndsInTheFramesAfterToo();
    AnEmptyFrameChangesNothing();
    ASmallDropStartsNothing();
    TheLongestCaptureEndsAndADropStillHoldingIsTheNewNormal();
    TheFirstFrameSetsTheBaseline();
    return lss::test::Report();
}
