// Host-side tests for each seat's off-screen arrow timers (hud/arrow_timers.h). Run without the game.

#include "hud/arrow_timers.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "arrow_timers: all passed\n" : "arrow_timers: %d failure(s)\n", g_failures);
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

using namespace lss::hud::arrow_timers;

const Key kA{1, 100};
const Key kB{2, 200};
const Key kAReused{1, 300};  // arrow A's widget back from the pool for another enemy

void NewArrowStartsAtZeroForTheSeat()
{
    SeatTimers seat;
    seat.BeginIn();
    EXPECT(seat.In(kA, 1.5f) == 0.0f);  // player 1's arrow is full; the seat has not seen it
    seat.BeginOut();
    const auto back = seat.Out(kA, 1.5f);  // the seat's run left it full (off its screen)
    EXPECT(back.has_value() && *back == 1.5f);
    seat.EndOut();
    EXPECT(seat.Count() == 1);
}

void SeatKeepsItsOwnTimerAcrossFrames()
{
    SeatTimers seat;
    seat.BeginIn();
    seat.In(kA, 0.0f);
    seat.BeginOut();
    seat.Out(kA, 1.0f);
    seat.EndOut();
    // Next frame: player 1's timer differs; the seat gets its own back, player 1's is restored after the run.
    seat.BeginIn();
    EXPECT(seat.In(kA, 0.25f) == 1.0f);
    seat.BeginOut();
    const auto back = seat.Out(kA, 0.9f);
    EXPECT(back.has_value() && *back == 0.25f);
    seat.EndOut();
    seat.BeginIn();
    EXPECT(seat.In(kA, 0.25f) == 0.9f);
}

void ArrowAddedByTheRunKeepsItsValue()
{
    SeatTimers seat;
    seat.BeginIn();
    seat.In(kA, 0.5f);
    seat.BeginOut();
    EXPECT(!seat.Out(kB, 0.0f).has_value());  // not there before the run: nothing of player 1's to put back
    seat.EndOut();
}

void ArrowsNotSeenAreDropped()
{
    SeatTimers seat;
    seat.BeginIn();
    seat.In(kA, 0.0f);
    seat.In(kB, 0.0f);
    seat.BeginOut();
    seat.Out(kA, 1.0f);
    seat.Out(kB, 1.0f);
    seat.EndOut();
    seat.BeginIn();
    seat.In(kA, 0.0f);
    seat.BeginOut();
    seat.Out(kA, 1.0f);  // B's enemy is gone
    seat.EndOut();
    EXPECT(seat.Count() == 1);
    seat.BeginIn();
    EXPECT(seat.In(kB, 0.0f) == 0.0f);
}

void ReusedWidgetIsANewArrow()
{
    SeatTimers seat;
    seat.BeginIn();
    seat.In(kA, 0.0f);
    seat.BeginOut();
    seat.Out(kA, 1.0f);
    seat.EndOut();
    seat.BeginIn();
    EXPECT(seat.In(kAReused, 0.0f) == 0.0f);
}

void ForgetDropsEverything()
{
    SeatTimers seat;
    seat.BeginIn();
    seat.In(kA, 0.0f);
    seat.BeginOut();
    seat.Out(kA, 1.0f);
    seat.EndOut();
    seat.Forget();
    EXPECT(seat.Count() == 0);
    seat.BeginIn();
    EXPECT(seat.In(kA, 0.0f) == 0.0f);
}

void OverflowStartsAtZeroAndIsCounted()
{
    SeatTimers seat;
    seat.BeginIn();
    seat.BeginOut();
    for (std::uint64_t i = 0; i < kCapacity + 3; ++i)
    {
        seat.Out(Key{i, i}, 1.0f);
    }
    seat.EndOut();
    EXPECT(seat.Count() == kCapacity);
    EXPECT(seat.Overflows() == 3);
    seat.BeginIn();
    EXPECT(seat.In(Key{kCapacity + 1, kCapacity + 1}, 0.0f) == 0.0f);
}

}  // namespace

int main()
{
    NewArrowStartsAtZeroForTheSeat();
    SeatKeepsItsOwnTimerAcrossFrames();
    ArrowAddedByTheRunKeepsItsValue();
    ArrowsNotSeenAreDropped();
    ReusedWidgetIsANewArrow();
    ForgetDropsEverything();
    OverflowStartsAtZeroAndIsCounted();
    return lss::test::Report();
}
