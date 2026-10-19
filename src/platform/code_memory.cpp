#include "platform/code_memory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cstring>

namespace lss::platform {

namespace {

constexpr std::uintptr_t kAllocationGrain = 0x10000;     // VirtualAlloc reserves on 64 KiB boundaries
constexpr std::uintptr_t kSearchRange     = 0x7FF00000;  // within a rel32 instruction's reach, with margin

}  // namespace

std::uintptr_t SearchCodePageNear(std::uintptr_t site, RegionQuery query, PageReserve reserve, void* context) noexcept
{
    const std::uintptr_t low   = site > kSearchRange + kAllocationGrain ? site - kSearchRange : kAllocationGrain;
    const std::uintptr_t high  = site + kSearchRange;
    const std::uintptr_t start = site & ~(kAllocationGrain - 1);

    // One cursor each way; each step takes the nearer one. An occupied region moves its cursor past
    // the whole region (to the first 64 KiB boundary beyond it), never by less than one grain.
    std::uintptr_t below     = start - kAllocationGrain;
    std::uintptr_t above     = start + kAllocationGrain;
    bool           belowDone = start < low + kAllocationGrain;
    bool           aboveDone = above > high;
    while (!belowDone || !aboveDone)
    {
        const bool           down      = !belowDone && (aboveDone || site - below <= above - site);
        const std::uintptr_t candidate = down ? below : above;
        MemoryRegion         region{};
        const bool           known = query(context, candidate, region);
        if (known && region.free)
        {
            if (const std::uintptr_t page = reserve(context, candidate))
            {
                return page;
            }
        }
        if (down)
        {
            // Below the region's base (its last grain boundary under it), or one grain down.
            std::uintptr_t next = candidate - kAllocationGrain;
            if (known && !region.free && region.base > kAllocationGrain)
            {
                next = (std::min)(next, (region.base - 1) & ~(kAllocationGrain - 1));
            }
            belowDone = next < low || next >= candidate || (known && !region.free && region.base <= kAllocationGrain);
            below     = next;
        }
        else
        {
            if (!known)
            {
                aboveDone = true;  // past the top of the address space
                continue;
            }
            std::uintptr_t next = candidate + kAllocationGrain;
            if (!region.free)
            {
                next = (std::max)(next, (region.base + region.size + kAllocationGrain - 1) & ~(kAllocationGrain - 1));
            }
            aboveDone = next > high || next <= candidate;
            above     = next;
        }
    }
    return 0;
}

namespace {

constexpr std::size_t kPoolBlock  = 0x10000;  // one allocation grain: what a search finds
constexpr std::size_t kPieceAlign = 16;
constexpr std::size_t kMaxBlocks  = 32;  // 2 MiB of placed code; the mod places ~30 KiB

struct PoolBlock
{
    std::uintptr_t base = 0;
    std::size_t    used = 0;
};

SRWLOCK                             g_poolLock = SRWLOCK_INIT;  // never throws, unlike std::mutex in noexcept code
std::array<PoolBlock, kMaxBlocks>   g_blocks{};
std::size_t                         g_blockCount = 0;
CodePoolCounts                      g_counts{};

bool WithinReach(std::uintptr_t site, std::uintptr_t address) noexcept
{
    return (address > site ? address - site : site - address) <= kSearchRange;
}

/// A new committed, read-write block within rel32 reach of `site`, or 0.
std::uintptr_t NewBlockNear(std::uintptr_t site) noexcept
{
    const RegionQuery query = [](void*, std::uintptr_t address, MemoryRegion& region) noexcept {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0)
        {
            return false;
        }
        region = {reinterpret_cast<std::uintptr_t>(info.BaseAddress), info.RegionSize, info.State == MEM_FREE};
        return true;
    };
    const PageReserve reserve = [](void*, std::uintptr_t candidate) noexcept {
        return reinterpret_cast<std::uintptr_t>(
            VirtualAlloc(reinterpret_cast<void*>(candidate), kPoolBlock, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    };
    const std::uintptr_t block = SearchCodePageNear(site, query, reserve, nullptr);
    // The search vouches for the block's start; its end must be in reach too.
    if (block != 0 && !WithinReach(site, block + kPoolBlock))
    {
        VirtualFree(reinterpret_cast<void*>(block), 0, MEM_RELEASE);
        return 0;
    }
    return block;
}

class PoolLock
{
public:
    PoolLock() noexcept { AcquireSRWLockExclusive(&g_poolLock); }
    ~PoolLock() { ReleaseSRWLockExclusive(&g_poolLock); }
    PoolLock(const PoolLock&)            = delete;
    PoolLock& operator=(const PoolLock&) = delete;
};

}  // namespace

std::uintptr_t PlaceCode(std::uintptr_t site, std::size_t size, CodeFill fill, void* context) noexcept
{
    if (size == 0 || size > kCodePageSize || fill == nullptr)
    {
        return 0;
    }
    const std::size_t rounded = (size + kPieceAlign - 1) & ~(kPieceAlign - 1);
    const PoolLock    lock;

    PoolBlock* block = nullptr;
    for (std::size_t i = 0; i < g_blockCount && block == nullptr; ++i)
    {
        PoolBlock& candidate = g_blocks[i];
        if (candidate.used + rounded <= kPoolBlock && WithinReach(site, candidate.base) &&
            WithinReach(site, candidate.base + kPoolBlock))
        {
            block = &candidate;
        }
    }
    if (block == nullptr)
    {
        const std::uintptr_t base = g_blockCount < kMaxBlocks ? NewBlockNear(site) : 0;
        if (base == 0)
        {
            return 0;
        }
        block  = &g_blocks[g_blockCount++];
        *block = {base, 0};
        ++g_counts.blocks;
    }

    const std::uintptr_t                    at = block->base + block->used;
    std::array<std::uint8_t, kCodePageSize> code{};
    if (!fill(context, at, std::span<std::uint8_t>(code.data(), size)))
    {
        return 0;
    }
    // Executable throughout: code placed earlier on these pages may be running on another thread.
    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &previous))
    {
        return 0;
    }
    std::memcpy(reinterpret_cast<void*>(at), code.data(), size);
    VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READ, &previous);  // if refused, still runnable
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
    block->used += rounded;
    ++g_counts.pieces;
    g_counts.bytes += size;
    return at;
}

CodePoolCounts CodePool() noexcept
{
    const PoolLock lock;
    return g_counts;
}

bool SwapCodeBlock(std::uintptr_t block, const CodeBlock& expected, const CodeBlock& desired) noexcept
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(block), kCodeBlockSize, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        return false;
    }

    LONG64 comparand[2];
    LONG64 exchange[2];
    std::memcpy(comparand, expected.data(), sizeof(comparand));
    std::memcpy(exchange, desired.data(), sizeof(exchange));
    const bool swapped =
        InterlockedCompareExchange128(reinterpret_cast<volatile LONG64*>(block), exchange[1], exchange[0], comparand) != 0;

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(block), kCodeBlockSize, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(block), kCodeBlockSize);
    return swapped;
}

}  // namespace lss::platform
