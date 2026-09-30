#pragma once

// The two memory operations the mod's code changes share (code_redirect.h, call_redirect.h):
//   - a page of our own within a rel32 instruction's reach of the game's code;
//   - an atomic swap of one aligned 16-byte block of the game's code, so a thread running that
//     code sees either the old bytes or the new ones, never a mix.

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::platform {

inline constexpr std::uintptr_t kCodeBlockSize = 16;
inline constexpr std::size_t    kCodePageSize  = 0x1000;
using CodeBlock                                = std::array<std::uint8_t, kCodeBlockSize>;

/// Commits one read-write page within rel32 reach of `site` (both directions), for the caller to
/// fill; 0 on failure. Seal it with SealCodePage before anything jumps to it.
std::uintptr_t AllocateCodePageNear(std::uintptr_t site) noexcept;

/// Makes a filled page executable and read-only. On failure the page is released and false is
/// returned. A sealed page is never freed: another thread may be running in it at any moment
/// after it is reachable.
bool SealCodePage(std::uintptr_t page) noexcept;

/// Releases a page that was never sealed or made reachable.
void ReleaseCodePage(std::uintptr_t page) noexcept;

/// Replaces the aligned 16-byte block at `block` with `desired` if it still holds `expected`, in
/// one atomic store, lifting the page's protection only around it. Returns false, changing
/// nothing, if the block holds anything else or cannot be made writable.
bool SwapCodeBlock(std::uintptr_t block, const CodeBlock& expected, const CodeBlock& desired) noexcept;

}  // namespace lss::platform
