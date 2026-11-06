// Host-side tests for the column model of the HUD passes (hud/overlay_pass.h): the entry name per column, the ring
// slot's acquire rule across N passes and the render thread's draw gate per view. Run without the game.

#include "hud/overlay_pass.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "overlay_pass: all passed\n" : "overlay_pass: %d failure(s)\n", g_failures);
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

using namespace lss::hud::overlay_pass;

void EntryNames()
{
    constexpr unsigned long long stock = 0x0000000000012345ull;  // number 0
    static_assert(EntryName(stock, kSingle) == stock, "the single render keeps the stock name");
    static_assert(EntryName(stock, 0) == stock, "column 0 is the game's own entry");
    static_assert(EntryName(stock, 1) == stock + (2ull << 32), "column 1 keeps number 2 (player 2's entry as built)");
    static_assert(EntryName(stock, 2) == stock + (3ull << 32), "column 2 takes number 3");
    EXPECT(EntryName(stock, 1) != EntryName(stock, 2));
    EXPECT((EntryName(stock, 1) & 0xFFFFFFFFull) == stock);  // the low dword (the name's index) is untouched
}

void AcquireRule()
{
    // The game's one render: the game's own acquire, whatever is held (nothing can be).
    EXPECT(AcquireAction(kSingle, false) == SlotAction::Acquire);
    EXPECT(AcquireAction(kSingle, true) == SlotAction::Acquire);
    // Two columns, as built for player 2: column 1 acquires and holds, column 0 consumes.
    EXPECT(AcquireAction(1, false) == SlotAction::AcquireAndHold);
    EXPECT(AcquireAction(0, true) == SlotAction::ConsumeHeld);
    // Three columns: column 2 first acquires and holds, column 1 takes the held slot, column 0 consumes it.
    EXPECT(AcquireAction(2, false) == SlotAction::AcquireAndHold);
    EXPECT(AcquireAction(1, true) == SlotAction::TakeHeld);
    EXPECT(AcquireAction(0, true) == SlotAction::ConsumeHeld);
    // A further pass whose render returned before its acquire: the next pass acquires (and holds) itself; column 0
    // with nothing held makes the game's own acquire.
    EXPECT(AcquireAction(0, false) == SlotAction::Acquire);
}

void DrawGate()
{
    // No split view identity, or an entry of no column (pools 0 and 1): nothing skipped.
    EXPECT(!DrawSkipped(-1, 1, true));
    EXPECT(!DrawSkipped(0, -1, true));
    EXPECT(!DrawSkipped(1, -1, false));
    // Each view draws its own column's entry.
    EXPECT(!DrawSkipped(0, 0, true));
    EXPECT(!DrawSkipped(1, 1, true));
    EXPECT(!DrawSkipped(2, 2, true));
    // Column 0's view never draws a further column's entry.
    EXPECT(DrawSkipped(0, 1, true));
    EXPECT(DrawSkipped(0, 1, false));
    EXPECT(DrawSkipped(0, 2, false));
    // A further view skips column 0's entry once the slot holds a further entry, and keeps it before (the frame from
    // before the image started).
    EXPECT(DrawSkipped(1, 0, true));
    EXPECT(!DrawSkipped(1, 0, false));
    // A further view never draws another further column's entry.
    EXPECT(DrawSkipped(2, 1, true));
    EXPECT(DrawSkipped(1, 2, false));
}

/// A mirror gets a HUD pass of its own only on a monitor of another shape than player 1's view.
void MirrorShapes()
{
    constexpr float kTotal = 2560.0f + 3440.0f + 2560.0f;  // a span of three monitors, 1440 tall
    constexpr float kNarrow = 2560.0f / kTotal, kWide = 3440.0f / kTotal;
    EXPECT(OtherShape(kWide, 1.0f, kNarrow, 1.0f));     // a mirror on the ultrawide, player 1 on a 16:9 monitor
    EXPECT(OtherShape(kNarrow, 1.0f, kWide, 1.0f));     // player 1 on the ultrawide, a mirror on a 16:9 monitor
    EXPECT(!OtherShape(kNarrow, 1.0f, kNarrow, 1.0f));  // two 16:9 monitors of the row
    EXPECT(!OtherShape(1.0f / 3.0f, 1.0f, 1.0f / 3.0f, 1.0f));  // three equal monitors
    // A pixel's rounding between equal monitors stays the same shape.
    EXPECT(!OtherShape(2561.0f / kTotal, 1.0f, kNarrow, 1.0f));
    // Unread sizes never ask for a pass.
    EXPECT(!OtherShape(0.0f, 1.0f, kNarrow, 1.0f));
    EXPECT(!OtherShape(kWide, 1.0f, 0.0f, 1.0f));
    EXPECT(!OtherShape(kWide, -1.0f, kNarrow, 1.0f));
}

}  // namespace

int main()
{
    static_assert(kMaxColumns == 3, "three views (plan/seats.md)");
    static_assert(kSingle < 0, "the single render is no column");
    static_assert(OtherShape(0.5f, 1.0f, 0.25f, 1.0f), "constexpr");
    EntryNames();
    AcquireRule();
    DrawGate();
    MirrorShapes();
    return lss::test::Report();
}
