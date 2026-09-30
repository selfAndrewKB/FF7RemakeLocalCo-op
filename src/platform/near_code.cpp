#include "platform/near_code.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstring>

namespace lss::platform {

namespace {

constexpr std::uintptr_t kAllocationGrain = 0x10000;     // VirtualAlloc reserves on 64 KiB boundaries
constexpr std::uintptr_t kSearchRange     = 0x7FF00000;  // within a rel32 instruction's reach, with margin

}  // namespace

std::uintptr_t AllocateCodePageNear(std::uintptr_t site) noexcept
{
    const std::uintptr_t low   = site > kSearchRange ? site - kSearchRange : kAllocationGrain;
    const std::uintptr_t high  = site + kSearchRange;
    const std::uintptr_t start = site & ~(kAllocationGrain - 1);

    for (std::uintptr_t distance = kAllocationGrain; distance < kSearchRange; distance += kAllocationGrain)
    {
        for (const bool below : {true, false})
        {
            if (below && distance > start)
            {
                continue;  // would wrap below address zero
            }
            const std::uintptr_t candidate = below ? start - distance : start + distance;
            if (candidate < low || candidate > high)
            {
                continue;
            }
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<void*>(candidate), &info, sizeof(info)) == 0 || info.State != MEM_FREE)
            {
                continue;
            }
            if (void* page = VirtualAlloc(reinterpret_cast<void*>(candidate), kCodePageSize,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            {
                return reinterpret_cast<std::uintptr_t>(page);
            }
        }
    }
    return 0;
}

bool SealCodePage(std::uintptr_t page) noexcept
{
    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(page), kCodePageSize, PAGE_EXECUTE_READ, &previous))
    {
        ReleaseCodePage(page);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(page), kCodePageSize);
    return true;
}

void ReleaseCodePage(std::uintptr_t page) noexcept
{
    VirtualFree(reinterpret_cast<void*>(page), 0, MEM_RELEASE);
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
