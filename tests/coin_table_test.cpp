// Host-side tests for the coin table (battle/coin_table.h): placing, finding, clearing, the mark, and eviction of
// the oldest coin among a key's four slots. Run without the game.

#include "battle/coin_table.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "coin_table: all passed\n" : "coin_table: %d failure(s)\n", g_failures);
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

using namespace lss::battle::coin_table;

constexpr std::uintptr_t kRude = 0x1F0000A0;  // 16-byte aligned, as object addresses are
constexpr std::uintptr_t kReno = 0x1F0001B0;

void EmptyAnswersNothing()
{
    CoinTable table;
    EXPECT(table.PlayerOf(kRude) == 0);
    EXPECT(table.Find(kRude) == kNoSlot);
    EXPECT(!table.Marked(kRude));
    table.Mark(kRude);  // nothing to mark
    EXPECT(!table.Marked(kRude));
}

void SetFindClear()
{
    CoinTable table;
    table.Set(kRude, 2, 100);
    table.Set(kReno, 4, 100);
    EXPECT(table.PlayerOf(kRude) == 2);
    EXPECT(table.PlayerOf(kReno) == 4);
    EXPECT(table.Find(kRude) != kNoSlot && table.Find(kRude) != table.Find(kReno));
    table.Set(kRude, 1, 101);  // its own slot again
    EXPECT(table.PlayerOf(kRude) == 1);
    table.Clear(kRude);
    EXPECT(table.PlayerOf(kRude) == 0);
    EXPECT(table.PlayerOf(kReno) == 4);
}

void MarkFollowsTheToss()
{
    CoinTable table;
    table.Set(kRude, 1, 100);
    EXPECT(!table.Marked(kRude));
    table.Mark(kRude);
    EXPECT(table.Marked(kRude));
    table.Set(kRude, 2, 105);  // a new toss starts unmarked
    EXPECT(!table.Marked(kRude));
    EXPECT(table.PlayerOf(kRude) == 2);
}

/// Four keys sharing Rude's four slots fill them; a fifth evicts the oldest, never a newer one.
void OldestIsEvicted()
{
    CoinTable table;
    // Keys whose first probe lands in Rude's slots: built from Rude's slot sequence by brute force.
    std::uintptr_t keys[5]{};
    std::size_t    found = 0;
    for (std::uintptr_t candidate = 0x10; found < 5 && candidate < 0x10000000; candidate += 0x10)
    {
        if (SlotOf(candidate, 0) == SlotOf(kRude, 0))
        {
            keys[found++] = candidate;
        }
    }
    EXPECT(found == 5);
    for (std::size_t i = 0; i < 4; ++i)
    {
        table.Set(keys[i], 1, static_cast<std::uint32_t>(10 + i));  // keys[0] is the oldest
    }
    for (std::size_t i = 0; i < 4; ++i)
    {
        EXPECT(table.PlayerOf(keys[i]) == 1);
    }
    table.Set(keys[4], 2, 20);
    EXPECT(table.PlayerOf(keys[4]) == 2);
    EXPECT(table.PlayerOf(keys[0]) == 0);  // evicted
    for (std::size_t i = 1; i < 4; ++i)
    {
        EXPECT(table.PlayerOf(keys[i]) == 1);
    }
}

void LowBitsHoldThePlayer()
{
    EXPECT((kRude & kMask) == 0);
    CoinTable table;
    table.Set(kRude, 3, 1);
    EXPECT(table.PlayerOf(kRude) == 3);
    EXPECT(table.PlayerOf(kRude + 1) == 0);  // another key
}

/// The 12:43:57 case: a cannon tossing every frame with the same answer writes one line, then the next differing toss
/// reports the repeats; the same answer seconds later is a line of its own.
void RepeatsWithinASecondAreCounted()
{
    CoinTable table;
    unsigned  repeated = 99;
    EXPECT(!table.Repeat(kRude, 0x12, 1000, repeated) && repeated == 0);  // no coin yet
    table.Set(kRude, 2, 1);
    EXPECT(!table.Repeat(kRude, 0x12, 1000, repeated) && repeated == 0);  // the first line
    table.Set(kRude, 2, 1);
    EXPECT(table.Repeat(kRude, 0x12, 1008, repeated));
    table.Set(kRude, 2, 1);
    EXPECT(table.Repeat(kRude, 0x12, 1016, repeated));
    table.Set(kRude, 1, 1);
    EXPECT(!table.Repeat(kRude, 0x11, 1024, repeated) && repeated == 2);  // differs: reports the two repeats
    table.Set(kRude, 1, 4);
    EXPECT(!table.Repeat(kRude, 0x11, 4000, repeated) && repeated == 0);  // the same answer seconds later: its own line
}

void ATakenSlotStartsItsLineOver()
{
    CoinTable      table;
    std::uintptr_t keys[5]{};
    std::size_t    found = 0;
    for (std::uintptr_t candidate = 0x10; found < 5 && candidate < 0x10000000; candidate += 0x10)
    {
        if (SlotOf(candidate, 0) == SlotOf(kRude, 0))
        {
            keys[found++] = candidate;
        }
    }
    EXPECT(found == 5);
    unsigned repeated = 0;
    for (std::size_t i = 0; i < 4; ++i)
    {
        table.Set(keys[i], 1, static_cast<std::uint32_t>(10 + i));
        table.Repeat(keys[i], 0x11, 500, repeated);
    }
    table.Set(keys[4], 1, 20);  // takes keys[0]'s slot
    EXPECT(!table.Repeat(keys[4], 0x11, 501, repeated) && repeated == 0);  // not a repeat of keys[0]'s line
}

}  // namespace

int main()
{
    EmptyAnswersNothing();
    SetFindClear();
    MarkFollowsTheToss();
    OldestIsEvicted();
    LowBitsHoldThePlayer();
    RepeatsWithinASecondAreCounted();
    ATakenSlotStartsItsLineOver();
    return lss::test::Report();
}
