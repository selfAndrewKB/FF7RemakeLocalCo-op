// Host-side tests for platform/code_memory: the search for a code page within rel32 reach of a site,
// over a made-up address space, and the real code pool near this test's own code.

#include "platform/code_memory.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace lss::platform;

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

constexpr std::uintptr_t kGrain = 0x10000;
constexpr std::uintptr_t kTop   = 0x7FFFFFFF0000;  // the made-up address space ends here

/// A made-up address space: occupied regions (sorted, not overlapping) with free space between them;
/// counts the queries, and refuses to reserve the addresses in `refused`.
struct Space
{
    std::vector<MemoryRegion>  taken;
    std::vector<std::uintptr_t> refused;
    int                        queries = 0;
};

bool Query(void* context, std::uintptr_t address, MemoryRegion& region) noexcept
{
    auto& space = *static_cast<Space*>(context);
    ++space.queries;
    if (address >= kTop)
    {
        return false;
    }
    std::uintptr_t freeFrom = 0;
    for (const MemoryRegion& taken : space.taken)
    {
        if (address < taken.base)
        {
            region = {freeFrom, taken.base - freeFrom, true};
            return true;
        }
        if (address < taken.base + taken.size)
        {
            region = taken;
            return true;
        }
        freeFrom = taken.base + taken.size;
    }
    region = {freeFrom, kTop - freeFrom, true};
    return true;
}

std::uintptr_t Reserve(void* context, std::uintptr_t candidate) noexcept
{
    for (const std::uintptr_t refused : static_cast<Space*>(context)->refused)
    {
        if (refused == candidate)
        {
            return 0;
        }
    }
    return candidate;
}

std::uintptr_t Search(Space& space, std::uintptr_t site)
{
    return SearchCodePageNear(site, &Query, &Reserve, &space);
}

/// The game's case: a 163 MB image cut into a few hundred regions (sections, page protections), with
/// occupied neighbours on both sides. The old 64 KiB walk needed ~2,400 queries here.
void TestCrossesTheImageByRegions()
{
    constexpr std::uintptr_t kImage = 0x7FF731870000;
    constexpr std::uintptr_t kSize  = 0x9BAD000;
    Space                    space;
    space.taken.push_back({kImage - 0x200000, 0x200000, false});  // just below: taken
    for (std::uintptr_t at = 0; at < kSize; at += 0x80000)
    {
        space.taken.push_back({kImage + at, std::min<std::uintptr_t>(0x80000, kSize - at), false});
    }
    space.taken.push_back({kImage + kSize, 0x3000, false});  // just above: taken, not grain-sized
    const std::uintptr_t site = kImage + 0x5000000;
    const std::uintptr_t page = Search(space, site);
    // Nearest free grain: below the image it is 0x5210000 away, above it (kImage + kSize + 0x3000, rounded up)
    // is 0x4BB0000 away.
    EXPECT(page == ((kImage + kSize + 0x3000 + kGrain - 1) & ~(kGrain - 1)));
    EXPECT(space.queries < 2 * 340 + 8);  // at most about one query per region on each side
    std::printf("  crossing the image: %d queries\n", space.queries);
}

/// A free grain right next to the site comes first, below before above at the same distance.
void TestNearestFirst()
{
    Space space;
    space.taken.push_back({0x40000000, 0x100000, false});
    // Below the region the first free grain is 0x3FFF0000 (0x90000 away); above it 0x40100000 (0x80000 away).
    EXPECT(Search(space, 0x40080000) == 0x40100000);
    Space onlyAbove;
    onlyAbove.taken.push_back({0x30000000, 0x10100000, false});  // 0x30000000..0x40100000
    EXPECT(Search(onlyAbove, 0x40080000) == 0x40100000);
}

/// A free grain that cannot be reserved is passed over.
void TestRefusedCandidate()
{
    Space space;
    space.taken.push_back({0x40000000, 0x100000, false});
    space.refused.push_back(0x40100000);
    // Next: 0x3FFF0000 below and 0x40110000 above, both 0x90000 away; below comes first at a tie.
    EXPECT(Search(space, 0x40080000) == 0x3FFF0000);
}

/// Nothing free within rel32 reach: 0.
void TestNothingInReach()
{
    Space space;
    space.taken.push_back({0x10000, 0x200000000, false});
    EXPECT(Search(space, 0x100000000) == 0);
    EXPECT(space.queries < 8);
}

/// A site near address zero searches only above it, without wrapping.
void TestLowSite()
{
    Space space;
    space.taken.push_back({0x10000, 0x40000, false});
    EXPECT(Search(space, 0x20000) == 0x50000);
}

/// The real pool: pieces near one site share one block, each within rel32 reach and holding its bytes; a
/// refused fill places nothing.
void TestPoolPlacesNearAndShares()
{
    const auto site   = reinterpret_cast<std::uintptr_t>(&TestPoolPlacesNearAndShares);
    const auto before = CodePool();
    std::uintptr_t first = PlaceCodeNear(site, 14, [](std::uintptr_t, std::span<std::uint8_t> code) noexcept {
        std::fill(code.begin(), code.end(), std::uint8_t{0xCC});
        return true;
    });
    std::uintptr_t refused = PlaceCodeNear(site, 32, [](std::uintptr_t, std::span<std::uint8_t>) noexcept { return false; });
    std::uintptr_t addressSeen = 0;
    std::uintptr_t second = PlaceCodeNear(site, 21, [&addressSeen](std::uintptr_t at, std::span<std::uint8_t> code) noexcept {
        addressSeen = at;
        std::fill(code.begin(), code.end(), std::uint8_t{0x90});
        return true;
    });
    const auto after = CodePool();
    EXPECT(first != 0 && second != 0);
    EXPECT(refused == 0);
    EXPECT(addressSeen == second);
    EXPECT(second == first + 16);  // the next 16-byte slot: the refused piece took nothing
    EXPECT(first % 16 == 0);
    const std::uintptr_t distance = first > site ? first - site : site - first;
    EXPECT(distance < 0x7FF00000);
    EXPECT(*reinterpret_cast<const std::uint8_t*>(first) == 0xCC);
    EXPECT(*reinterpret_cast<const std::uint8_t*>(second + 20) == 0x90);
    EXPECT(after.blocks == before.blocks + 1);  // one search for both
    EXPECT(after.pieces == before.pieces + 2);
    EXPECT(after.bytes == before.bytes + 35);
    // Too big for one piece: refused.
    EXPECT(PlaceCodeNear(site, kCodePageSize + 1, [](std::uintptr_t, std::span<std::uint8_t>) noexcept { return true; }) == 0);
}

/// A site out of reach of the first block gets a block of its own.
void TestPoolFarSiteGetsItsOwnBlock()
{
    const auto site   = reinterpret_cast<std::uintptr_t>(&TestPoolFarSiteGetsItsOwnBlock);
    const auto far    = site + 0x100000000;  // 4 GB up: outside rel32 reach of anything near this test
    const auto before = CodePool();
    const std::uintptr_t placed = PlaceCodeNear(far, 16, [](std::uintptr_t, std::span<std::uint8_t> code) noexcept {
        std::fill(code.begin(), code.end(), std::uint8_t{0xC3});
        return true;
    });
    if (placed == 0)
    {
        std::printf("  (no free memory within reach of %#llx on this machine; skipped)\n",
                    static_cast<unsigned long long>(far));
        return;
    }
    const std::uintptr_t distance = placed > far ? placed - far : far - placed;
    EXPECT(distance < 0x7FF00000);
    EXPECT(CodePool().blocks == before.blocks + 1);
}
}  // namespace

int main()
{
    TestCrossesTheImageByRegions();
    TestNearestFirst();
    TestRefusedCandidate();
    TestNothingInReach();
    TestLowSite();
    TestPoolPlacesNearAndShares();
    TestPoolFarSiteGetsItsOwnBlock();
    if (g_failures != 0)
    {
        std::printf("code_memory: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("code_memory: all passed\n");
    return 0;
}
