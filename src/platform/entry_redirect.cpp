#include "platform/entry_redirect.h"

#include "platform/module_pin.h"
#include "platform/x64_jump.h"

#include <array>
#include <cstring>

namespace lss::platform {

namespace {

/// jmp qword ptr [rip+0] ; dq <address>
constexpr std::uint8_t kAbsoluteJump[]  = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
constexpr std::size_t  kTrampolineStart = 0x10;  // after the thunk's 14 bytes

constexpr std::size_t  kMaxCopied       = 2 * kCodeBlockSize;  // the instructions a trampoline replays

/// Code placed near `entry` (the shared code pool): at +0 a jump to `replacement`; at +0x10 the
/// trampoline (`copied`, the function's instructions from its entry up to and including the replaced
/// one, then a jump to the instruction after them). Returns its address, or 0.
/// Without `replay`, only the jump to `replacement`.
std::uintptr_t BuildPage(std::uintptr_t entry, std::span<const std::uint8_t> copied, std::uintptr_t replacement,
                         bool replay) noexcept
{
    if (!replay)
    {
        return PlaceCodeNear(entry, sizeof(kAbsoluteJump) + sizeof(replacement),
                             [replacement](std::uintptr_t, std::span<std::uint8_t> code) noexcept {
                                 std::memcpy(code.data(), kAbsoluteJump, sizeof(kAbsoluteJump));
                                 std::memcpy(code.data() + sizeof(kAbsoluteJump), &replacement, sizeof(replacement));
                                 return true;
                             });
    }
    return PlaceCodeNear(
        entry, kTrampolineStart + copied.size() + kJumpLength,
        [entry, copied, replacement](std::uintptr_t at, std::span<std::uint8_t> code) noexcept {
            const auto back = EncodeJump(at + kTrampolineStart + copied.size(), entry + copied.size());
            if (!back)
            {
                return false;
            }
            std::memcpy(code.data(), kAbsoluteJump, sizeof(kAbsoluteJump));
            std::memcpy(code.data() + sizeof(kAbsoluteJump), &replacement, sizeof(replacement));
            std::memcpy(code.data() + kTrampolineStart, copied.data(), copied.size());
            std::memcpy(code.data() + kTrampolineStart + copied.size(), back->data(), back->size());
            return true;
        });
}

}  // namespace

bool EntryRedirect::Install(std::uintptr_t entry, std::span<const std::uint8_t> first, std::uintptr_t replacement) noexcept
{
    return first.size() <= kMaxFirstBytes && Place(entry, first, 0, replacement);
}

bool EntryRedirect::InstallWithoutOriginal(std::uintptr_t entry, std::span<const std::uint8_t> first,
                                           std::uintptr_t replacement) noexcept
{
    return first.size() <= kMaxFirstBytes && Place(entry, first, 0, replacement, false);
}

bool EntryRedirect::InstallAtSecondInstruction(std::uintptr_t entry, std::span<const std::uint8_t> first,
                                               std::span<const std::uint8_t> second, std::uintptr_t replacement) noexcept
{
    if (first.empty() || second.size() > kMaxFirstBytes || first.size() + second.size() > kMaxCopied)
    {
        return false;
    }
    // The first instruction may cross a block: it is only read and copied, never written.
    if (std::memcmp(reinterpret_cast<const void*>(entry), first.data(), first.size()) != 0)
    {
        return false;
    }
    std::array<std::uint8_t, kMaxCopied> copied{};
    std::memcpy(copied.data(), first.data(), first.size());
    std::memcpy(copied.data() + first.size(), second.data(), second.size());
    return Place(entry, std::span<const std::uint8_t>(copied.data(), first.size() + second.size()), first.size(),
                 replacement);
}

bool EntryRedirect::Place(std::uintptr_t entry, std::span<const std::uint8_t> copied, std::size_t siteOffset,
                          std::uintptr_t replacement, bool replay) noexcept
{
    // The replaced instruction: the last of `copied`, at entry + siteOffset.
    const std::uintptr_t site     = entry + siteOffset;
    const std::size_t    replaced = copied.size() - siteOffset;
    const std::uintptr_t block    = site & ~(kCodeBlockSize - 1);
    const std::size_t    at       = site - block;
    // The thunk jumps into our compiled code, so the DLL must outlive any chance of the function
    // being entered (platform/module_pin.h).
    if (block_ != 0 || replacement == 0 || siteOffset >= copied.size() || replaced < kJumpLength ||
        at + replaced > kCodeBlockSize || !PinThisModule())
    {
        return false;
    }

    CodeBlock current{};
    std::memcpy(current.data(), reinterpret_cast<const void*>(block), current.size());
    if (std::memcmp(current.data() + at, copied.data() + siteOffset, replaced) != 0)
    {
        return false;
    }

    const std::uintptr_t page = BuildPage(entry, copied, replacement, replay);
    const auto           jump = page != 0 ? EncodeJump(site, page) : std::nullopt;
    if (!jump)
    {
        return false;  // placed code is left unused: never reached, never freed
    }

    CodeBlock redirected = current;
    std::memcpy(redirected.data() + at, jump->data(), jump->size());
    std::memset(redirected.data() + at + jump->size(), 0x90, replaced - jump->size());  // nop, never run
    trampoline_ = replay ? page + kTrampolineStart : 0;  // before the swap: the replacement may run the moment it is reachable
    if (!SwapCodeBlock(block, current, redirected))
    {
        trampoline_ = 0;
        return false;
    }
    block_      = block;
    original_   = current;
    redirected_ = redirected;
    return true;
}

bool EntryRedirect::Restore() noexcept
{
    if (block_ == 0)
    {
        return true;
    }
    if (!SwapCodeBlock(block_, redirected_, original_))
    {
        return false;
    }
    block_ = 0;
    return true;
}

}  // namespace lss::platform
