#include "platform/call_redirect.h"

#include "platform/log.h"
#include "platform/module_pin.h"

#include <array>
#include <cstring>
#include <limits>
#include <optional>

namespace lss::platform {

namespace {

constexpr std::uint8_t kCallOpcode = 0xE8;
constexpr std::uint8_t kJumpOpcode = 0xE9;  // jmp rel32: same length and displacement rule as the call
constexpr std::size_t  kCallLength = 5;

/// jmp qword ptr [rip+0] ; dq <address>
constexpr std::uint8_t kAbsoluteJump[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};

std::uintptr_t CallTarget(std::uintptr_t site, const std::uint8_t* bytes) noexcept
{
    std::int32_t rel = 0;
    std::memcpy(&rel, bytes + 1, sizeof(rel));
    return site + kCallLength + static_cast<std::intptr_t>(rel);
}

std::optional<std::int32_t> Displacement(std::uintptr_t site, std::uintptr_t to) noexcept
{
    const auto displacement = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(site + kCallLength);
    if (displacement < std::numeric_limits<std::int32_t>::min() || displacement > std::numeric_limits<std::int32_t>::max())
    {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(displacement);
}

/// A sealed page near `site` holding a jump to `replacement`, or 0.
std::uintptr_t BuildThunk(std::uintptr_t site, std::uintptr_t replacement) noexcept
{
    const std::uintptr_t page = AllocateCodePageNear(site);
    if (page == 0)
    {
        return 0;
    }
    std::memcpy(reinterpret_cast<void*>(page), kAbsoluteJump, sizeof(kAbsoluteJump));
    std::memcpy(reinterpret_cast<void*>(page + sizeof(kAbsoluteJump)), &replacement, sizeof(replacement));
    return SealCodePage(page) ? page : 0;
}

}  // namespace

bool CallRedirect::Install(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement) noexcept
{
    return Place(site, target, replacement, false, kCallOpcode);
}

bool CallRedirect::InstallTailJump(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement) noexcept
{
    return Place(site, target, replacement, false, kJumpOpcode);
}

bool CallRedirect::InstallFromOwnThread(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement) noexcept
{
    return Place(site, target, replacement, true, kCallOpcode);
}

bool CallRedirect::Place(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement, bool crossingAllowed,
                         std::uint8_t opcode) noexcept
{
    const std::uintptr_t block   = site & ~(kCodeBlockSize - 1);
    const std::size_t    at      = site - block;
    const bool           crosses = at + kCallLength > kCodeBlockSize;
    // The thunk jumps into our compiled code, so the DLL must outlive any chance of the call being
    // taken (platform/module_pin.h).
    if (block_ != 0 || replacement == 0 || (crosses && !crossingAllowed) || !PinThisModule())
    {
        return false;
    }

    // The call's bytes, read as one run over both blocks when it crosses.
    std::array<std::uint8_t, 2 * kCodeBlockSize> current{};
    std::memcpy(current.data(), reinterpret_cast<const void*>(block), crosses ? current.size() : kCodeBlockSize);
    if (current[at] != opcode || CallTarget(site, current.data() + at) != target)
    {
        return false;
    }

    const std::uintptr_t thunk = BuildThunk(site, replacement);
    const auto           rel   = thunk != 0 ? Displacement(site, thunk) : std::nullopt;
    if (!rel)
    {
        return false;  // a built thunk is left allocated and unused: never reached, never freed
    }

    auto redirected = current;
    std::memcpy(redirected.data() + at + 1, &*rel, sizeof(*rel));
    CodeBlock first{}, firstRedirected{}, second{}, secondRedirected{};
    std::memcpy(first.data(), current.data(), kCodeBlockSize);
    std::memcpy(firstRedirected.data(), redirected.data(), kCodeBlockSize);
    std::memcpy(second.data(), current.data() + kCodeBlockSize, kCodeBlockSize);
    std::memcpy(secondRedirected.data(), redirected.data() + kCodeBlockSize, kCodeBlockSize);

    target_ = target;  // before the swap: the replacement may run the moment the call reaches it
    if (!SwapCodeBlock(block, first, firstRedirected))
    {
        target_ = 0;
        return false;
    }
    if (crosses && !SwapCodeBlock(block + kCodeBlockSize, second, secondRedirected))
    {
        // Back to the game's bytes. This can fail only if another writer changed the first block since
        // our swap on this thread; the call is then torn, and the log says so.
        if (!SwapCodeBlock(block, firstRedirected, first))
        {
            Log(L"FAILED: a redirected call at {:#x} could not be put back after its second block refused; the call is torn",
                site);
        }
        target_ = 0;
        return false;
    }
    block_          = block;
    original_       = first;
    redirected_     = firstRedirected;
    crosses_        = crosses;
    nextOriginal_   = second;
    nextRedirected_ = secondRedirected;
    return true;
}

bool CallRedirect::Restore() noexcept
{
    if (block_ == 0)
    {
        return true;
    }
    if (!SwapCodeBlock(block_, redirected_, original_))
    {
        return false;
    }
    if (crosses_ && !SwapCodeBlock(block_ + kCodeBlockSize, nextRedirected_, nextOriginal_))
    {
        // Keep the call whole: ours, as it was (fails only if another writer changed the first block).
        if (!SwapCodeBlock(block_, original_, redirected_))
        {
            Log(L"FAILED: a redirected call in the block at {:#x} is torn: its second block was changed after us",
                block_);
        }
        return false;
    }
    block_   = 0;
    crosses_ = false;
    return true;
}

}  // namespace lss::platform
