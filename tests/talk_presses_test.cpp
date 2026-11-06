// Host-side tests for the talk-press table (world/talk_presses.h): the six rules of a further player's NPC press
// waiting for its act, as world/talk_presses keeps them. Run without the game.

#include "world/talk_presses.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "talk_presses: all passed\n" : "talk_presses: %d failure(s)\n", g_failures);
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

using namespace lss::world::talk_presses;

constexpr std::uint64_t kNpcA = 0x1001, kNpcB = 0x1002;

/// Rule 1: a newer press of the same seat replaces the older; another seat's press stands beside it; the table fills.
void NewerPressReplaces()
{
    Table   table;
    Dropped dropped;
    EXPECT(table.Note(1, kNpcA, 100, dropped) != nullptr);
    EXPECT(dropped.count == 0);
    EXPECT(table.Peek(kNpcA) == 1);
    EXPECT(table.Note(1, kNpcB, 200, dropped) != nullptr);
    EXPECT(dropped.count == 1 && dropped.presses[0].name == kNpcA && dropped.presses[0].seat == 1);
    EXPECT(table.Peek(kNpcA) == 0);
    EXPECT(table.Peek(kNpcB) == 1);
    Dropped none;
    EXPECT(table.Note(2, kNpcA, 300, none) != nullptr);
    EXPECT(none.count == 0);  // seat 2 had none to drop
    EXPECT(table.Peek(kNpcA) == 2 && table.Peek(kNpcB) == 1);
    // The capacity: distinct seats (unbuilt ones, so no replacement) fill the table; a new seat is refused, while a
    // seat with a press still gets its slot, its older press replaced.
    Table   crowded;
    Dropped d;
    for (std::size_t i = 0; i < kCapacity; ++i)
    {
        EXPECT(crowded.Note(static_cast<int>(10 + i), 0x3000 + i, 10, d) != nullptr);
    }
    EXPECT(crowded.Note(99, 0x4000, 10, d) == nullptr);
    Dropped replaced;
    EXPECT(crowded.Note(10, 0x4001, 11, replaced) != nullptr);
    EXPECT(replaced.count == 1 && replaced.presses[0].name == 0x3000);
}

/// Rule 2: player 1's press of the same object drops every seat's press of it, other objects' presses stay.
void PlayerOnePressDrops()
{
    Table   table;
    Dropped d;
    table.Note(1, kNpcA, 1, d);
    table.Note(2, kNpcA, 2, d);  // both seats pressed the same NPC
    Dropped dropped;
    table.DropOf(kNpcA, dropped);
    EXPECT(dropped.count == 2 && !table.Any());  // every seat's press of it
    Table other;
    other.Note(1, kNpcA, 1, d);
    other.Note(2, kNpcB, 2, d);
    Dropped kept;
    other.DropOf(kNpcA, kept);
    EXPECT(kept.count == 1 && kept.presses[0].seat == 1);
    EXPECT(other.Peek(kNpcA) == 0 && other.Peek(kNpcB) == 2);  // another object's press stays
}

/// Rule 3: a world change drops all; a seat's leave drops that seat's.
void WorldChangeAndLeaveDrop()
{
    Table   table;
    Dropped d;
    table.Note(1, kNpcA, 1, d);
    table.Note(2, kNpcB, 1, d);
    Dropped left;
    table.DropSeat(2, left);
    EXPECT(left.count == 1 && left.presses[0].name == kNpcB);
    EXPECT(table.Peek(kNpcB) == 0 && table.Peek(kNpcA) == 1);
    Dropped all;
    table.DropAll(all);
    EXPECT(all.count == 1 && !table.Any());
}

/// Rule 4: the wait counts only while no lock holds; the lock restarts it.
void LapseOnlyUnlocked()
{
    Table   table;
    Dropped d;
    table.Note(1, kNpcA, 1000, d);
    Dropped lapsed;
    table.Tick(1000 + kLapseMs, false, lapsed);  // exactly the wait: not yet over
    EXPECT(lapsed.count == 0 && table.Peek(kNpcA) == 1);
    table.Tick(1000 + kLapseMs + 1, true, lapsed);  // locked: the wait restarts from here
    EXPECT(lapsed.count == 0 && table.Peek(kNpcA) == 1);
    table.Tick(1000 + 2 * kLapseMs, false, lapsed);  // within the restarted wait
    EXPECT(lapsed.count == 0);
    table.Tick(1000 + 2 * kLapseMs + 2, false, lapsed);  // over it
    EXPECT(lapsed.count == 1 && lapsed.presses[0].name == kNpcA);
    EXPECT(table.Peek(kNpcA) == 0);
}

/// Rule 5: the spot noted at the press stays with it; unknown answers false.
void SpotKept()
{
    Table   table;
    Dropped d;
    Press*  press = table.Note(1, kNpcA, 1, d);
    std::array<float, 3> spot{};
    EXPECT(!table.Spot(kNpcA, spot));
    press->spot      = {1.0f, 2.0f, 3.0f};
    press->spotKnown = true;
    EXPECT(table.Spot(kNpcA, spot) && spot[0] == 1.0f && spot[1] == 2.0f && spot[2] == 3.0f);
    EXPECT(!table.Spot(kNpcB, spot));
}

/// Rule 6: a judged press stays judged; a claim takes every seat's press of the object and names Peek's seat.
void JudgedAndClaimed()
{
    Table   table;
    Dropped d;
    table.Note(1, kNpcA, 1, d);
    table.Note(2, kNpcA, 2, d);  // both seats pressed the same NPC
    EXPECT(!table.Judged(kNpcA));
    table.MarkJudged(kNpcA);
    EXPECT(table.Judged(kNpcA));
    EXPECT(table.Peek(kNpcA) == 1);  // the first noted
    EXPECT(table.Claim(kNpcA) == 1);
    EXPECT(table.Peek(kNpcA) == 0 && !table.Any());  // seat 2's press of it taken with it: one act answers one press
    EXPECT(table.Claim(kNpcA) == 0);
    EXPECT(table.Claim(0) == 0);
}

/// A press of a seat that is not built is never answered.
void UnbuiltSeatNeverAnswered()
{
    Table   table;
    Dropped d;
    table.Note(7, kNpcA, 1, d);
    EXPECT(table.Any());
    EXPECT(table.Peek(kNpcA) == 0);
    std::array<float, 3> spot{};
    EXPECT(!table.Spot(kNpcA, spot));
}

}  // namespace

int main()
{
    NewerPressReplaces();
    PlayerOnePressDrops();
    WorldChangeAndLeaveDrop();
    LapseOnlyUnlocked();
    SpotKept();
    JudgedAndClaimed();
    UnbuiltSeatNeverAnswered();
    return lss::test::Report();
}
