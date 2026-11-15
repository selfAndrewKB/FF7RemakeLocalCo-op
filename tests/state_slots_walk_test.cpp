// Host-side tests for state_slots::ForEach (world/state_slots.h): how a walk of the state manager's active list ends,
// on a list laid out in host memory as the game lays it out (a node's next at +0x08 and its slot index at +0x18, the slot
// pointers in the array). The game ends its list with a null next; a census must call that whole. Run without the game.

#include "world/state_slots.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "state_slots_walk: all passed\n" : "state_slots_walk: %d failure(s)\n", g_failures);
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

using namespace lss;
using world::state_slots::ForEach;
using world::state_slots::Walk;

struct Node
{
    std::uint8_t bytes[0x20]{};
};

/// A state manager in host memory: the slot array and the list's head and list object at their offsets.
struct Manager
{
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(facts::kStateManagerSlots + 8u * 4u, 0);
    std::uint8_t              slots[4][0x20]{};
    Node                      nodes[3];

    std::uint8_t* Sm() { return memory.data(); }

    template <typename T>
    static void Put(std::uint8_t* at, T value)
    {
        std::memcpy(at, &value, sizeof(T));
    }

    /// Three nodes for slots 0, 1 and 2; the last node's next is `last`.
    explicit Manager(const void* last)
    {
        for (int i = 0; i < 4; ++i)
        {
            Put(Sm() + facts::kStateManagerSlots + 8u * i, static_cast<void*>(slots[i]));
        }
        Put(Sm() + facts::kStateManagerActiveHead, static_cast<void*>(&nodes[0]));
        for (int i = 0; i < 3; ++i)
        {
            Put(nodes[i].bytes + facts::kStateNodeSlotIndex, static_cast<std::int32_t>(i));
            Put(nodes[i].bytes + facts::kStateNodeNext, i < 2 ? static_cast<const void*>(&nodes[i + 1]) : last);
        }
    }
};

/// Walks every slot of `m`'s list: the walk's end and the slots seen.
Walk WalkAll(Manager& m, int& seen)
{
    seen = 0;
    return ForEach(m.Sm(), [&](const std::uint8_t*) noexcept {
        ++seen;
        return false;
    });
}

void TestAListEndingInNullIsWhole()
{
    Manager m(nullptr);
    int     seen = 0;
    EXPECT(WalkAll(m, seen) == Walk::Ended);
    EXPECT(seen == 3);
}

void TestAListBackAtItsObjectIsWhole()
{
    Manager m(nullptr);
    Manager::Put(m.nodes[2].bytes + facts::kStateNodeNext, static_cast<const void*>(m.Sm() + facts::kStateManagerActiveList));
    int seen = 0;
    EXPECT(WalkAll(m, seen) == Walk::Ended);
    EXPECT(seen == 3);
}

void TestAStopIsStopped()
{
    Manager m(nullptr);
    int     seen = 0;
    EXPECT(ForEach(m.Sm(), [&](const std::uint8_t*) noexcept { return ++seen == 2; }) == Walk::Stopped);
    EXPECT(seen == 2);
}

void TestABadIndexOrNullSlotIsCut()
{
    int     seen = 0;
    Manager bad(nullptr);
    Manager::Put(bad.nodes[1].bytes + facts::kStateNodeSlotIndex, static_cast<std::int32_t>(facts::kStateManagerSlotCount));
    EXPECT(WalkAll(bad, seen) == Walk::Cut && seen == 2);

    Manager empty(nullptr);
    Manager::Put(empty.Sm() + facts::kStateManagerSlots + 8u, static_cast<void*>(nullptr));
    EXPECT(WalkAll(empty, seen) == Walk::Cut && seen == 2);

    // A next the walk cannot read.
    Manager unread(nullptr);
    Manager::Put(unread.nodes[1].bytes + facts::kStateNodeNext, reinterpret_cast<const void*>(std::uintptr_t{1}));
    EXPECT(WalkAll(unread, seen) == Walk::Cut);
}

}  // namespace

int main()
{
    TestAListEndingInNullIsWhole();
    TestAListBackAtItsObjectIsWhole();
    TestAStopIsStopped();
    TestABadIndexOrNullSlotIsCut();
    return lss::test::Report();
}
