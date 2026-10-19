#include "platform/instruction_patch.h"

#include <cstring>

namespace lss::platform {

bool InstructionPatch::Apply(std::uintptr_t site, std::span<const std::uint8_t> original,
                             std::span<const std::uint8_t> replacement) noexcept
{
    const std::uintptr_t block = site & ~(kCodeBlockSize - 1);
    const std::size_t    at    = site - block;
    if (block_ != 0 || original.empty() || original.size() != replacement.size() ||
        at + original.size() > kCodeBlockSize)
    {
        return false;
    }

    CodeBlock current{};
    std::memcpy(current.data(), reinterpret_cast<const void*>(block), current.size());
    if (std::memcmp(current.data() + at, original.data(), original.size()) != 0)
    {
        return false;
    }

    CodeBlock patched = current;
    std::memcpy(patched.data() + at, replacement.data(), replacement.size());
    if (!SwapCodeBlock(block, current, patched))
    {
        return false;
    }
    block_    = block;
    original_ = current;
    patched_  = patched;
    return true;
}

bool InstructionPatch::Restore() noexcept
{
    if (block_ == 0)
    {
        return true;
    }
    if (!SwapCodeBlock(block_, patched_, original_))
    {
        return false;
    }
    block_ = 0;
    return true;
}

}  // namespace lss::platform
