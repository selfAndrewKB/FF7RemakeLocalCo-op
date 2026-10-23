// Host-side tests for the navi map's pool-0 handover rule (hud/minimap_rules.h). Run without the game.

#include "hud/minimap_rules.h"
#include "hud/overlay_pass.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "minimap_rules: all passed\n" : "minimap_rules: %d failure(s)\n", g_failures);
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

using lss::hud::minimap_rules::EntryName;
using lss::hud::minimap_rules::PoolHandover;
using Take = PoolHandover::Take;

constexpr int kSingle = lss::hud::overlay_pass::kSingle;  // the game's one render, no split

/// One player, or player 1's pass with nothing held: the game's own acquire and submit, the most common frame.
void NothingHeld()
{
    PoolHandover pool;
    EXPECT(pool.OnAcquire(kSingle, 0) == Take::Game);
    EXPECT(!pool.SkipSubmit(kSingle, false));
    EXPECT(pool.OnAcquire(0, 0) == Take::Game);
    EXPECT(!pool.SkipSubmit(0, false));
    EXPECT(!pool.Held());
    EXPECT(!pool.EndFrame());
}

/// Two columns: player 2's pass acquires and holds, player 1's is handed the slot and sends it.
void TwoColumns()
{
    PoolHandover pool;
    EXPECT(pool.OnAcquire(1, 1) == Take::Acquire);
    EXPECT(pool.Held());
    EXPECT(pool.SkipSubmit(1, true));
    EXPECT(pool.Held());
    EXPECT(pool.OnAcquire(0, 0) == Take::Held);
    EXPECT(!pool.SkipSubmit(0, true));
    EXPECT(!pool.Held());
    EXPECT(!pool.EndFrame());
}

/// Three columns: the highest further column acquires, the next is handed the slot and skips its submit, player 1's
/// is handed it and sends it.
void ThreeColumns()
{
    PoolHandover pool;
    EXPECT(pool.OnAcquire(2, 2) == Take::Acquire);
    EXPECT(pool.SkipSubmit(2, true));
    EXPECT(pool.OnAcquire(1, 1) == Take::Held);
    EXPECT(pool.SkipSubmit(1, true));
    EXPECT(pool.Held());
    EXPECT(pool.OnAcquire(0, 0) == Take::Held);
    EXPECT(!pool.SkipSubmit(0, true));
    EXPECT(!pool.Held());
    EXPECT(!pool.EndFrame());
}

/// Another painter, or an octagon in a pass not its own, gets the game's acquire and submit untouched.
void OtherPainters()
{
    PoolHandover pool;
    EXPECT(pool.OnAcquire(1, -1) == Take::Game);  // a second map widget in player 2's pass
    EXPECT(pool.OnAcquire(1, 0) == Take::Game);   // player 1's octagon in player 2's pass (it is skipped there anyway)
    EXPECT(pool.OnAcquire(0, 1) == Take::Game);   // player 2's octagon in player 1's pass
    EXPECT(pool.OnAcquire(1, 2) == Take::Game);   // player 3's octagon in player 2's pass, and the other way round
    EXPECT(pool.OnAcquire(2, 1) == Take::Game);
    EXPECT(pool.OnAcquire(kSingle, 0) == Take::Game);
    EXPECT(pool.OnAcquire(kSingle, 1) == Take::Game);
    EXPECT(!pool.Held());
    EXPECT(!pool.SkipSubmit(1, false));
    EXPECT(!pool.SkipSubmit(0, false));
    EXPECT(!pool.EndFrame());
    // Held: another painter still gets the game's acquire, and the held slot stays held.
    EXPECT(pool.OnAcquire(1, 1) == Take::Acquire);
    EXPECT(pool.OnAcquire(1, -1) == Take::Game);
    EXPECT(pool.OnAcquire(1, 2) == Take::Game);
    EXPECT(!pool.SkipSubmit(1, false));
    EXPECT(pool.Held());
    EXPECT(pool.OnAcquire(0, -1) == Take::Game);
    EXPECT(pool.OnAcquire(0, 1) == Take::Game);
    EXPECT(pool.OnAcquire(0, 0) == Take::Held);
    EXPECT(!pool.SkipSubmit(0, true));
}

/// The column holding the slot acquires again in the same pass: the held slot is sent first, the new one held. A
/// column handed the slot paints twice: handed it both times, nothing resent.
void SameColumnTwice()
{
    PoolHandover pool;
    EXPECT(pool.OnAcquire(1, 1) == Take::Acquire);
    EXPECT(pool.OnAcquire(1, 1) == Take::ResendThenAcquire);
    EXPECT(pool.Held());
    EXPECT(pool.OnAcquire(0, 0) == Take::Held);
    EXPECT(!pool.SkipSubmit(0, true));
    EXPECT(!pool.Held());

    PoolHandover three;
    EXPECT(three.OnAcquire(2, 2) == Take::Acquire);
    EXPECT(three.OnAcquire(1, 1) == Take::Held);
    EXPECT(three.OnAcquire(1, 1) == Take::Held);
    EXPECT(three.OnAcquire(0, 0) == Take::Held);
    EXPECT(!three.SkipSubmit(0, true));
    EXPECT(!three.Held());
}

/// The held slot submitted outside the split's passes (the game's one render): sent and released.
void SingleRenderSends()
{
    PoolHandover pool;
    EXPECT(pool.OnAcquire(1, 1) == Take::Acquire);
    EXPECT(!pool.SkipSubmit(kSingle, true));
    EXPECT(!pool.Held());
    EXPECT(!pool.EndFrame());
}

/// Player 1's map is not painted this frame: the slot is still held at the frame's end, and the frame after starts
/// clean.
void MapNotPainted()
{
    PoolHandover pool;
    EXPECT(pool.OnAcquire(1, 1) == Take::Acquire);
    EXPECT(pool.SkipSubmit(1, true));
    EXPECT(pool.EndFrame());
    EXPECT(!pool.Held());
    EXPECT(!pool.EndFrame());
    EXPECT(pool.OnAcquire(1, 1) == Take::Acquire);
    EXPECT(pool.OnAcquire(0, 0) == Take::Held);
}

/// A submit of a slot that is not the held one never changes the hold.
void OtherSubmits()
{
    PoolHandover pool;
    EXPECT(pool.OnAcquire(2, 2) == Take::Acquire);
    EXPECT(!pool.SkipSubmit(2, false));
    EXPECT(!pool.SkipSubmit(0, false));
    EXPECT(pool.Held());
    EXPECT(!pool.SkipSubmit(0, true));
    EXPECT(!pool.Held());
}

/// Entry names: player 1's unchanged, a further column's with its number in the FName's number part.
void EntryNames()
{
    const unsigned long long name = 0x0000000012345678ull;
    EXPECT(EntryName(name, 0) == name);
    EXPECT(EntryName(name, kSingle) == name);
    EXPECT(EntryName(name, 1) == name + (2ull << 32));
    EXPECT(EntryName(name, 2) == name + (3ull << 32));
}

}  // namespace

int main()
{
    NothingHeld();
    TwoColumns();
    ThreeColumns();
    OtherPainters();
    SameColumnTwice();
    SingleRenderSends();
    MapNotPainted();
    OtherSubmits();
    EntryNames();
    return lss::test::Report();
}
