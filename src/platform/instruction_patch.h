#pragma once

// Rewrites one instruction of the game's code in place, with one of the same length, and puts it back.
//
// For a fix that is a different operand, not different code (render/distortion_apply.h: one stack offset), a
// redirect's jump and code page would only add moving parts. The rules are those of the redirects
// (code_redirect.h):
//   - the replaced bytes are compared against what the research read before anything is written;
//   - they must lie inside one aligned 16-byte block, swapped in a single atomic store (code_memory.h), so a
//     thread running that code sees either the old instruction or the new one, never a mix;
//   - the replacement has the original's length, so every instruction keeps its start: a thread stopped
//     anywhere in the block resumes on an instruction boundary either way.
// The destructor leaves an applied patch in place, as the redirects do; only Restore() removes it.

#include "platform/code_memory.h"

#include <cstdint>
#include <span>

namespace lss::platform {

class InstructionPatch
{
public:
    InstructionPatch() = default;
    ~InstructionPatch() = default;  // leaves an applied patch in place (see above)

    InstructionPatch(const InstructionPatch&)            = delete;
    InstructionPatch& operator=(const InstructionPatch&) = delete;

    /// Replaces `original` at `site` (live address; one instruction, inside one aligned 16-byte block) with
    /// `replacement` of the same length. Returns false, changing nothing, if any condition fails.
    bool Apply(std::uintptr_t site, std::span<const std::uint8_t> original,
               std::span<const std::uint8_t> replacement) noexcept;

    /// Puts the original instruction back, if this object replaced it and it is still ours.
    /// Returns false if it was changed after us (it is then left alone).
    bool Restore() noexcept;

    bool IsApplied() const noexcept { return block_ != 0; }

private:
    std::uintptr_t block_ = 0;  // the aligned 16-byte block holding the site
    CodeBlock      original_{};
    CodeBlock      patched_{};
};

}  // namespace lss::platform
