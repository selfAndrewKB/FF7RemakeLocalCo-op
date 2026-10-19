#include "platform/code_redirect.h"

#include "platform/code_memory.h"
#include "platform/x64_jump.h"

#include <cstring>

namespace lss::platform {

namespace {

/// Places `code` then a jump to `resume` near `site` (the shared code pool). Returns its address, or 0.
std::uintptr_t BuildCodePage(std::uintptr_t site, std::span<const std::uint8_t> code, std::uintptr_t resume) noexcept
{
    return PlaceCodeNear(site, code.size() + kJumpLength,
                         [code, resume](std::uintptr_t at, std::span<std::uint8_t> placed) noexcept {
                             const auto back = EncodeJump(at + code.size(), resume);
                             if (!back)
                             {
                                 return false;
                             }
                             std::memcpy(placed.data(), code.data(), code.size());
                             std::memcpy(placed.data() + code.size(), back->data(), back->size());
                             return true;
                         });
}

}  // namespace

bool CodeRedirect::Install(std::uintptr_t site, std::span<const std::uint8_t> replaced,
                           std::span<const std::uint8_t> code, std::uintptr_t resume) noexcept
{
    const std::uintptr_t block = site & ~(kCodeBlockSize - 1);
    const std::size_t    at    = site - block;
    if (block_ != 0 || replaced.size() < kJumpLength || at + replaced.size() > kCodeBlockSize)
    {
        return false;
    }

    Block current{};
    std::memcpy(current.data(), reinterpret_cast<const void*>(block), current.size());
    if (std::memcmp(current.data() + at, replaced.data(), replaced.size()) != 0)
    {
        return false;
    }

    const std::uintptr_t page = BuildCodePage(site, code, resume);
    const auto           jump = page != 0 ? EncodeJump(site, page) : std::nullopt;
    if (!jump)
    {
        return false;  // placed code is left unused: never reached, never freed
    }

    Block redirected = current;
    std::memcpy(redirected.data() + at, jump->data(), jump->size());
    std::memset(redirected.data() + at + jump->size(), 0x90, replaced.size() - jump->size());  // nop

    if (!SwapCodeBlock(block, current, redirected))
    {
        return false;
    }
    block_      = block;
    original_   = current;
    redirected_ = redirected;
    return true;
}

bool CodeRedirect::Restore() noexcept
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
