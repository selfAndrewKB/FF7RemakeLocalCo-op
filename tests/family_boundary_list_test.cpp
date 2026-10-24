// Host-side tests for render/family_boundary_list.h: the node append the game inlines, and the default-context check.

#include "render/family_boundary_list.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

namespace b = lss::render::boundary;

std::uint64_t Address(const void* p)
{
    return reinterpret_cast<std::uint64_t>(p);
}

void NodeFn(void*, void*) {}

// The grow: hands out a fresh block, as the game's allocator rewrites the arena's cursor and end.
alignas(16) std::array<std::byte, 0x100> g_block{};
int          g_grows   = 0;
std::int32_t g_request = 0;
void Grow(void* arena, std::int32_t bytes)
{
    ++g_grows;
    g_request = bytes;
    const std::uint64_t start = Address(g_block.data()) + 3;  // unaligned on purpose: the append aligns it
    const std::uint64_t end   = Address(g_block.data()) + g_block.size();
    std::memcpy(arena, &start, 8);
    std::memcpy(static_cast<std::byte*>(arena) + 8, &end, 8);
}

struct List
{
    alignas(16) std::array<std::byte, 0x48> bytes{};
    std::uint64_t Get(std::size_t at) const
    {
        std::uint64_t v = 0;
        std::memcpy(&v, bytes.data() + at, 8);
        return v;
    }
    void Put(std::size_t at, std::uint64_t v) { std::memcpy(bytes.data() + at, &v, 8); }
};

void TestAppend()
{
    std::puts("append: an empty list grows, links Root, counts; the second node links the first; Next 0, fn at +8");
    List list;
    list.Put(b::list::kCommandLink, Address(list.bytes.data()));  // &Root when empty
    const auto first = b::AppendNode(list.bytes.data(), &NodeFn, &Grow);
    EXPECT(g_grows == 1 && g_request == 0x18);
    EXPECT(first % 8 == 0 && first >= Address(g_block.data()) + 3);
    EXPECT(list.Get(0) == first);                          // Root
    EXPECT(list.Get(b::list::kCommandLink) == first);      // the last node (its Next at +0)
    EXPECT(list.Get(b::list::kArena) == first + 0x10);     // cursor past the node
    std::uint32_t count = 0;
    std::memcpy(&count, list.bytes.data() + b::list::kNumCommands, 4);
    EXPECT(count == 1);
    std::uint64_t next = 1, fn = 0;
    std::memcpy(&next, reinterpret_cast<void*>(first), 8);
    std::memcpy(&fn, reinterpret_cast<void*>(first + 8), 8);
    EXPECT(next == 0 && fn == Address(reinterpret_cast<const void*>(&NodeFn)));

    // A cursor 4 bytes past a node: aligned up, no grow while it fits.
    list.Put(b::list::kArena, first + 0x14);
    const auto second = b::AppendNode(list.bytes.data(), &NodeFn, &Grow);
    EXPECT(g_grows == 1 && second == first + 0x18);
    std::memcpy(&next, reinterpret_cast<void*>(first), 8);
    EXPECT(next == second && list.Get(b::list::kCommandLink) == second && list.Get(0) == first);
    std::memcpy(&count, list.bytes.data() + b::list::kNumCommands, 4);
    EXPECT(count == 2);

    // Not enough room before the end: grows.
    list.Put(b::list::kArena + 8, second + 0x10 + 8);  // end: 8 bytes after the cursor
    const auto third = b::AppendNode(list.bytes.data(), &NodeFn, &Grow);
    EXPECT(g_grows == 2 && third % 8 == 0);
    std::memcpy(&next, reinterpret_cast<void*>(second), 8);
    EXPECT(next == third);
}

void TestContext()
{
    std::puts("context: the default direct one only (vtable, not async, the device's first direct context)");
    constexpr std::uintptr_t vtable = 0x1234;
    std::vector<std::byte> ctx(0x2B00), other(0x2B00), device(0x700);
    std::array<std::uint64_t, 2> direct{Address(ctx.data()), Address(other.data())};
    const auto put = [](std::vector<std::byte>& at, std::size_t offset, std::uint64_t v) { std::memcpy(at.data() + offset, &v, 8); };
    for (auto* c : {&ctx, &other})
    {
        put(*c, 0, vtable);
        put(*c, b::context::kDevice, Address(device.data()));
    }
    put(device, b::context::kDeviceDirect, Address(direct.data()));
    EXPECT(b::IsDefaultDirectContext(ctx.data(), vtable));
    EXPECT(!b::IsDefaultDirectContext(other.data(), vtable));  // a parallel context
    EXPECT(!b::IsDefaultDirectContext(ctx.data(), vtable + 8));  // the redirector's vtable
    ctx[b::context::kIsAsyncCompute] = std::byte{1};
    EXPECT(!b::IsDefaultDirectContext(ctx.data(), vtable));
    ctx[b::context::kIsAsyncCompute] = std::byte{0};
    put(device, b::context::kDeviceDirect, 0);
    EXPECT(!b::IsDefaultDirectContext(ctx.data(), vtable));
    EXPECT(!b::IsDefaultDirectContext(nullptr, vtable));
}

}  // namespace

int main()
{
    TestAppend();
    TestContext();
    if (g_failures != 0)
    {
        std::printf("family_boundary_list: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("family_boundary_list: all passed\n");
    return 0;
}
