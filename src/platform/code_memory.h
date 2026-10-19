#pragma once

// The two memory operations the mod's code changes share (code_redirect.h, call_redirect.h):
//   - code of our own within a rel32 instruction's reach of the game's code (a shared pool);
//   - an atomic swap of one aligned 16-byte block of the game's code, so a thread running that
//     code sees either the old bytes or the new ones, never a mix.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::platform {

inline constexpr std::uintptr_t kCodeBlockSize = 16;
inline constexpr std::size_t    kCodePageSize  = 0x1000;
using CodeBlock                                = std::array<std::uint8_t, kCodeBlockSize>;

/// Places `size` bytes of code (at most kCodePageSize) within rel32 reach of `site` and returns their
/// address (16-byte aligned), or 0. `fill` writes the bytes into `code` once their address is known
/// (for jumps relative to it); returning false places nothing.
/// The code goes into a shared pool of executable 64 KiB blocks: one search finds a block near the
/// game's image and every later site within its reach reuses it, so a redirect costs no search after
/// the first (a search per redirect cost ~100 ms across the game's fragmented image, live 2026-10-07).
/// Placed code is never freed: another thread may run it at any moment after it is reachable. A block
/// stays executable while code is added to it, so code already in it keeps running. `fill` runs under
/// the pool's lock, which is not re-entrant: it must only write bytes, never place code itself.
using CodeFill = bool (*)(void* context, std::uintptr_t address, std::span<std::uint8_t> code) noexcept;
std::uintptr_t PlaceCode(std::uintptr_t site, std::size_t size, CodeFill fill, void* context) noexcept;

/// PlaceCode with a callable `bool(std::uintptr_t address, std::span<std::uint8_t> code) noexcept`.
template <class Fill>
std::uintptr_t PlaceCodeNear(std::uintptr_t site, std::size_t size, Fill fill) noexcept
{
    return PlaceCode(
        site, size,
        [](void* context, std::uintptr_t address, std::span<std::uint8_t> code) noexcept {
            return (*static_cast<Fill*>(context))(address, code);
        },
        &fill);
}

/// What the pool holds so far (for the log).
struct CodePoolCounts
{
    std::size_t blocks = 0;   // 64 KiB blocks found by a search
    std::size_t pieces = 0;   // pieces of code placed
    std::size_t bytes  = 0;   // their bytes
};
CodePoolCounts CodePool() noexcept;

/// One region of the address space as VirtualQuery reports it: [base, base + size), all free or all not.
struct MemoryRegion
{
    std::uintptr_t base = 0;
    std::uintptr_t size = 0;
    bool           free = false;
};

/// The search behind the pool's blocks, with the address space passed in (host tests). It tries the
/// 64 KiB-aligned addresses within rel32 reach of `site`, nearest first, alternating below and above,
/// and passes over each occupied region in one step. The game's image is still ~1,200 regions (its
/// patched and relocated pages each split one off), so a search from inside it costs ~100 ms: the pool
/// above makes it a once-per-launch cost.
/// `query` fills the region around an address (false: none, e.g. past the top of the address space);
/// `reserve` returns the page committed at a free candidate, or 0 to go on searching.
using RegionQuery = bool (*)(void* context, std::uintptr_t address, MemoryRegion& region) noexcept;
using PageReserve = std::uintptr_t (*)(void* context, std::uintptr_t candidate) noexcept;
std::uintptr_t SearchCodePageNear(std::uintptr_t site, RegionQuery query, PageReserve reserve, void* context) noexcept;

/// Replaces the aligned 16-byte block at `block` with `desired` if it still holds `expected`, in
/// one atomic store, lifting the page's protection only around it. Returns false, changing
/// nothing, if the block holds anything else or cannot be made writable.
bool SwapCodeBlock(std::uintptr_t block, const CodeBlock& expected, const CodeBlock& desired) noexcept;

}  // namespace lss::platform
