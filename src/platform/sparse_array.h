#pragma once

// Walks the allocated elements of an Unreal TSparseArray (the element array of a TSet or TMap) by reading memory only.
// Taken from Rebirth (cleanup-2026-10 platform/sparse_array.h, proven on its HUD sets); the bound became a required
// argument, since Remake's first user walks a level's data tables (thousands of rows), not a HUD set.
//
// Stock UE 4.18 layout, x64, the same as Rebirth's 4.26 (read in this build's own walk, UEndDataObjectAction__GetRowNames
// 0x141988BD0, RE-data-table-rows.md):
//   +0x00 Data (element pointer), +0x08 ArrayNum,
//   +0x10 allocation bits, inline (4 dwords), +0x20 their heap pointer (used when non-null),
//   +0x28 NumBits.
// An element is allocated when its bit is set. Every read is guarded; a walk that cannot read the header visits
// nothing. Game thread only (the sets belong to game-thread systems).

#include "platform/guarded.h"

#include <cstdint>

namespace lss::platform {

inline constexpr std::uint32_t kSparseArrayData       = 0x00;
inline constexpr std::uint32_t kSparseArrayInlineBits = 0x10;
inline constexpr std::uint32_t kSparseArrayHeapBits   = 0x20;
inline constexpr std::uint32_t kSparseArrayNumBits    = 0x28;
inline constexpr std::int32_t  kSparseArrayInlineMax  = 128;  // 4 inline dwords

/// Calls `visit(elementAddress)` for each allocated element of the TSparseArray at `array`, in index order, until it
/// returns false. A bit count over `maxBits`, or over the inline words with no heap bits, is a bad read: nothing is
/// visited. Returns false if nothing could be walked. May throw what `visit` throws.
template <class Visit>
bool ForEachSparseElement(std::uintptr_t array, std::uint32_t elementSize, std::int32_t maxBits, Visit&& visit)
{
    std::uintptr_t data = 0, heapBits = 0;
    std::int32_t   numBits = 0;
    if (array == 0 || !CopyGuarded(&data, reinterpret_cast<const void*>(array + kSparseArrayData), sizeof(data)) || data == 0 ||
        !CopyGuarded(&heapBits, reinterpret_cast<const void*>(array + kSparseArrayHeapBits), sizeof(heapBits)) ||
        !CopyGuarded(&numBits, reinterpret_cast<const void*>(array + kSparseArrayNumBits), sizeof(numBits)) || numBits <= 0 ||
        numBits > maxBits || (heapBits == 0 && numBits > kSparseArrayInlineMax))
    {
        return false;
    }
    const std::uintptr_t bits = heapBits != 0 ? heapBits : array + kSparseArrayInlineBits;
    for (std::int32_t base = 0; base < numBits; base += 32)
    {
        // One read per 32 slots; a word with no bit set is skipped whole.
        std::uint32_t word = 0;
        if (!CopyGuarded(&word, reinterpret_cast<const void*>(bits + sizeof(std::uint32_t) * static_cast<std::uint32_t>(base >> 5)),
                         sizeof(word)))
        {
            continue;
        }
        for (std::int32_t i = base; word != 0 && i < numBits && i < base + 32; ++i)
        {
            const std::uint32_t bit = 1u << (i & 31);
            if ((word & bit) == 0)
            {
                continue;  // a free slot
            }
            word &= ~bit;
            if (!visit(data + static_cast<std::uintptr_t>(i) * elementSize))
            {
                return true;
            }
        }
    }
    return true;
}

}  // namespace lss::platform
