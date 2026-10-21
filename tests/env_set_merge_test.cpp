// Host-side tests for env_set_merge (world/env_set_merge.h): which of player 2's EnvironmentSet entries join player 1's,
// the levels handed to the apply, player 2's kept set and when the game must apply again. Run without the game.
//
// The case from the orphanage (2026-10-21): player 1 out on the pathway (set EnvSet_AerithHousePathway, which does not
// name the pictures), player 2 at the picture (a set hiding BG0717_01_01 and BG0717_02_01). Names here are made-up
// indices; only equality matters.

#include "world/env_set_merge.h"

#include <cstdio>
#include <initializer_list>
#include <span>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "env_set_merge: all passed\n" : "env_set_merge: %d failure(s)\n", g_failures);
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

using namespace lss::world::env_set_merge;

constexpr Name kPictureOne{101, 0};
constexpr Name kPictureTwo{102, 0};
constexpr Name kLamp{200, 0};
constexpr Name kLevelOrphanage{300, 0};
constexpr Name kLevelPathway{301, 0};

EntryList ListOf(std::initializer_list<Entry> entries)
{
    EntryList list;
    for (const Entry& entry : entries)
    {
        list.Add(entry);
    }
    return list;
}

void TestExtrasAddOnlyWhatPlayerOneDoesNotName()
{
    const EntryList one = ListOf({{kLamp, 1}});
    const EntryList two = ListOf({{kPictureOne, 1}, {kPictureTwo, 1}, {kLamp, 0}});
    EntryList       out;
    EXPECT(Extras(one.View(), two.View(), out));
    EXPECT(out.count == 2);
    EXPECT(out.items[0] == (Entry{kPictureOne, 1}));
    EXPECT(out.items[1] == (Entry{kPictureTwo, 1}));  // the lamp keeps player 1's flag: not added
}

void TestExtrasSameNameDifferentNumber()
{
    const EntryList one = ListOf({{{101, 1}, 1}});  // BG0717_01_01 vs BG0717_01_00: same index, other number
    const EntryList two = ListOf({{kPictureOne, 1}});
    EntryList       out;
    EXPECT(Extras(one.View(), two.View(), out));
    EXPECT(out.count == 1);
}

void TestExtrasEmptyAndRepeats()
{
    EntryList out;
    out.Add({kLamp, 1});  // cleared first
    EXPECT(Extras({}, {}, out));
    EXPECT(out.count == 0);
    const EntryList two = ListOf({{kPictureOne, 1}, {kPictureOne, 0}});
    EXPECT(Extras({}, two.View(), out));
    EXPECT(out.count == 1 && out.items[0].hidden == 1);
}

void TestExtrasOverflow()
{
    EntryList two;
    for (std::uint32_t i = 0; i < kMaxEntries; ++i)
    {
        two.Add({{i + 1, 0}, 1});
    }
    EXPECT(!two.Add({{9999, 0}, 1}));  // the list itself is bounded
    EntryList out;
    EXPECT(Extras({}, two.View(), out));
    EXPECT(out.count == kMaxEntries);
}

void TestLevels()
{
    List<Name, 4> out;
    out.Add(kLevelPathway);
    const Name two[] = {kLevelOrphanage, kLevelPathway, kLevelOrphanage};
    EXPECT(AddMissingLevels(std::span<const Name>{}, two, out));
    EXPECT(out.count == 2 && out.items[1] == kLevelOrphanage);  // the one already there and the repeat skipped
    const Name more[] = {{1, 0}, {2, 0}, {3, 0}};
    EXPECT(!AddMissingLevels(std::span<const Name>{}, more, out));  // full
}

void TestTwoSetKeptOnNoHit()
{
    TwoSet last;
    last.entries.Add({kPictureOne, 1});
    TwoSet fresh;
    fresh.entries.Add({kLamp, 1});
    EXPECT(NextTwoSet(true, true, fresh, last).entries == fresh.entries);
    EXPECT(NextTwoSet(true, false, fresh, last).entries == last.entries);  // no set under player 2: the last one
    EXPECT(NextTwoSet(false, true, fresh, last).entries.count == 0);      // out or off: nothing
    EXPECT(NextTwoSet(false, false, fresh, last).entries.count == 0);
}

void TestMustApply()
{
    const EntryList none;
    const EntryList pictures = ListOf({{kPictureOne, 1}, {kPictureTwo, 1}});
    EXPECT(!MustApply(none, none));             // nothing added, nothing applied: the game alone
    EXPECT(MustApply(pictures, none));          // player 2 walks up to the picture
    EXPECT(!MustApply(pictures, pictures));     // and stays there: no re-apply each frame
    EXPECT(MustApply(none, pictures));          // player 2 walks away or leaves: the game puts them back
    EXPECT(MustApply(ListOf({{kPictureOne, 1}}), pictures));
    EXPECT(MustApply(ListOf({{kPictureOne, 1}, {kPictureTwo, 0}}), pictures));  // a flag changed
}

}  // namespace

int main()
{
    TestExtrasAddOnlyWhatPlayerOneDoesNotName();
    TestExtrasSameNameDifferentNumber();
    TestExtrasEmptyAndRepeats();
    TestExtrasOverflow();
    TestLevels();
    TestTwoSetKeptOnNoHit();
    TestMustApply();
    return lss::test::Report();
}
