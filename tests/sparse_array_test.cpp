// Host-side tests for platform/sparse_array.h: the walk over a TSparseArray's allocated elements, on a fake array laid
// out as UE 4.18's (data +0, inline bits +0x10, heap bits +0x20, NumBits +0x28). Run without the game.

#include "platform/sparse_array.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "sparse_array: all passed\n" : "sparse_array: %d failure(s)\n", g_failures);
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

using lss::platform::ForEachSparseElement;

struct FakeArray
{
    std::uintptr_t data = 0;
    std::int32_t   num = 0, max = 0;
    std::uint32_t  inlineBits[4]{};
    std::uintptr_t heapBits = 0;
    std::int32_t   numBits = 0, maxBits = 0;
};
static_assert(offsetof(FakeArray, inlineBits) == 0x10 && offsetof(FakeArray, heapBits) == 0x20 && offsetof(FakeArray, numBits) == 0x28,
              "the fake must have UE 4.18's layout");

constexpr std::uint32_t kStride = 16;

/// The indices visited (from element addresses), and the walk's result.
struct Visited
{
    std::vector<std::int32_t> indices;
    bool                      result = false;
};

Visited Walk(const FakeArray& array, std::int32_t maxBits, std::int32_t stopAfter = -1)
{
    Visited v;
    v.result = ForEachSparseElement(reinterpret_cast<std::uintptr_t>(&array), kStride, maxBits, [&](std::uintptr_t element) {
        v.indices.push_back(static_cast<std::int32_t>((element - array.data) / kStride));
        return stopAfter < 0 || static_cast<std::int32_t>(v.indices.size()) < stopAfter;
    });
    return v;
}

std::vector<std::uint8_t> g_elements(256 * kStride);

void TestInlineBitsVisitAllocatedInOrder()
{
    FakeArray a{.data = reinterpret_cast<std::uintptr_t>(g_elements.data()), .num = 40, .numBits = 40};
    a.inlineBits[0] = (1u << 0) | (1u << 3) | (1u << 31);
    a.inlineBits[1] = 1u << 7;  // index 39
    const Visited v = Walk(a, 4096);
    EXPECT(v.result);
    EXPECT((v.indices == std::vector<std::int32_t>{0, 3, 31, 39}));
}

void TestHeapBitsWinWhenSet()
{
    std::vector<std::uint32_t> heap(8, 0);
    heap[5] = 1u << 2;  // index 162
    FakeArray a{.data = reinterpret_cast<std::uintptr_t>(g_elements.data()), .num = 200, .numBits = 200};
    a.inlineBits[0] = 1u;  // ignored: the heap bits are set
    a.heapBits      = reinterpret_cast<std::uintptr_t>(heap.data());
    const Visited v = Walk(a, 4096);
    EXPECT(v.result);
    EXPECT((v.indices == std::vector<std::int32_t>{162}));
}

void TestAnEmptyArrayIsRead()
{
    const FakeArray a{};  // no data, no bits
    const Visited   v = Walk(a, 4096);
    EXPECT(v.result);
    EXPECT(v.indices.empty());
}

void TestBadCountsAreRefused()
{
    FakeArray a{.data = reinterpret_cast<std::uintptr_t>(g_elements.data()), .numBits = -1};
    EXPECT(!Walk(a, 4096).result);
    a.numBits = 300;  // over the bound
    EXPECT(!Walk(a, 256).result);
    a.numBits = 129;  // over the inline words, no heap bits
    EXPECT(!Walk(a, 4096).result);
    a.numBits = 10;
    a.data    = 0;  // a count with no data
    EXPECT(!Walk(a, 4096).result);
}

void TestVisitStopsTheWalk()
{
    FakeArray a{.data = reinterpret_cast<std::uintptr_t>(g_elements.data()), .num = 8, .numBits = 8};
    a.inlineBits[0] = 0xFFu;
    const Visited v = Walk(a, 4096, 2);
    EXPECT(v.result);  // stopped by the visit, not a bad read
    EXPECT((v.indices == std::vector<std::int32_t>{0, 1}));
}

void TestAnUnreadableBitWordStops()
{
    FakeArray a{.data = reinterpret_cast<std::uintptr_t>(g_elements.data()), .num = 200, .numBits = 200};
    a.heapBits      = 0x10;  // the null page: the guarded read fails
    const Visited v = Walk(a, 4096);
    EXPECT(!v.result);
    EXPECT(v.indices.empty());
}

}  // namespace

int main()
{
    TestInlineBitsVisitAllocatedInOrder();
    TestHeapBitsWinWhenSet();
    TestAnEmptyArrayIsRead();
    TestBadCountsAreRefused();
    TestVisitStopsTheWalk();
    TestAnUnreadableBitWordStops();
    return lss::test::Report();
}
