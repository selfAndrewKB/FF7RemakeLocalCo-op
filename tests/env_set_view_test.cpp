// Host-side tests for env_set_view (world/env_set_view.h): player 2's kept set, its pacing, and the edit of a hidden-actor
// list that touches only the mod's own entries. Run without the game.
//
// The case from the orphanage (2026-10-21): player 1 out on the pathway (set EnvSet_AerithHousePathway, which does not
// name the pictures), player 2 at the picture (a set hiding BG0717_01_01 and BG0717_02_01). Names here are made-up
// indices; only equality matters.

#include "world/env_set_view.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "env_set_view: all passed\n" : "env_set_view: %d failure(s)\n", g_failures);
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

using namespace lss::world::env_set_view;

constexpr Name kPictureOne{101, 0};
constexpr Name kLamp{200, 0};
constexpr Name kLevelOrphanage{300, 0};

TwoSet SetOf(Name actor, std::uint32_t hidden)
{
    TwoSet set;
    set.entries.Add({actor, hidden});
    set.levels.Add(kLevelOrphanage);
    return set;
}

void TestTwoSetKeptOnNoHit()
{
    const TwoSet last  = SetOf(kPictureOne, 1);
    const TwoSet fresh = SetOf(kLamp, 1);
    EXPECT(NextTwoSet(true, true, fresh, last) == fresh);
    EXPECT(NextTwoSet(true, false, fresh, last) == last);  // no set under player 2: the last one
    EXPECT(NextTwoSet(false, true, fresh, last).entries.count == 0);  // out or off: nothing
    EXPECT(NextTwoSet(false, false, fresh, last).entries.count == 0);
}

void TestPaceTakesTheFirstChangeAtOnce()
{
    Paced paced;
    EXPECT(Pace(paced, SetOf(kPictureOne, 1), 10.0));  // the first set is taken at once
    EXPECT(paced.set == SetOf(kPictureOne, 1));
    EXPECT(!Pace(paced, SetOf(kPictureOne, 1), 10.1));  // the same set: no change
}

void TestPaceHoldsAtAnEdge()
{
    // The ground under player 2 names two sets in turn every few frames (v0.32.0, live: 80 ms).
    Paced paced;
    const TwoSet a = SetOf(kPictureOne, 1), b = SetOf(kLamp, 1);
    EXPECT(Pace(paced, a, 0.0));
    EXPECT(!Pace(paced, b, 0.08));
    EXPECT(!Pace(paced, a, 0.16));
    EXPECT(!Pace(paced, b, 0.99));
    EXPECT(paced.set == a);
    EXPECT(Pace(paced, b, 1.0));  // the pause has run out: the set under player 2 now
    EXPECT(paced.set == b);
    EXPECT(!Pace(paced, b, 5.0));
}

void TestPaceSeesAFlagChange()
{
    Paced paced;
    EXPECT(Pace(paced, SetOf(kPictureOne, 1), 0.0));
    EXPECT(Pace(paced, SetOf(kPictureOne, 0), 2.0));  // same actor, now shown
}

void TestPaceDropsAtOnceWhenOut()
{
    Paced paced;
    EXPECT(Pace(paced, SetOf(kPictureOne, 1), 0.0));
    EXPECT(!Pace(paced, TwoSet{}, 0.2));             // at an edge the pause holds the set
    EXPECT(Pace(paced, TwoSet{}, 0.2, true));        // out, or the game's hold mode: dropped at once
    EXPECT(paced.set.levels.count == 0 && paced.set.entries.count == 0);
    EXPECT(!Pace(paced, SetOf(kPictureOne, 1), 0.5));  // back in: the pause runs from the drop
    EXPECT(Pace(paced, SetOf(kPictureOne, 1), 1.2));
}

void TestTwoSetSeesAByteChange()
{
    TwoSet a = SetOf(kPictureOne, 1);
    TwoSet b = a;
    a.hidden.Add(0);
    b.hidden.Add(1);
    EXPECT(!(a == b));  // the same level, hidden in one: a change (world/area_levels loads by it)
}

void TestRemoveOursKeepsOthersInOrder()
{
    int   a = 0, b = 0, c = 0, d = 0;
    void* items[] = {&a, &b, &c, &d, &b};
    void* const ours[] = {&b, &d};
    const int kept = RemoveOurs(items, 5, ours);
    EXPECT(kept == 2);
    EXPECT(items[0] == &a && items[1] == &c);
    EXPECT(RemoveOurs(items, kept, {}) == 2);  // nothing of ours: unchanged
    EXPECT(RemoveOurs(items, 0, ours) == 0);
}

void TestBounds()
{
    EntryList list;
    for (int i = 0; i < kMaxEntries; ++i)
    {
        EXPECT(list.Add({{static_cast<std::uint32_t>(i + 1), 0}, 1}));
    }
    EXPECT(!list.Add({{9999, 0}, 1}));  // bounded, never past the storage
    EXPECT(list.count == kMaxEntries);
}

}  // namespace

int main()
{
    TestTwoSetKeptOnNoHit();
    TestPaceTakesTheFirstChangeAtOnce();
    TestPaceHoldsAtAnEdge();
    TestPaceSeesAFlagChange();
    TestPaceDropsAtOnceWhenOut();
    TestTwoSetSeesAByteChange();
    TestRemoveOursKeepsOthersInOrder();
    TestBounds();
    return lss::test::Report();
}
