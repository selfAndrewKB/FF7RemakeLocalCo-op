#pragma once

// Sends execution at one verified spot in the game's code through a few bytes of our own, and
// puts the original bytes back.
//
// The mod otherwise changes the game only through a vtable entry (vtable_entry.h). This exists for
// the one decision that has no entry to replace: the final output pass deciding whether to clear
// the screen (renderer/additional_family_output.h). The shape is kept as small as a redirect can be:
//   - the replaced bytes are compared against what the research read before anything is written;
//   - they must lie inside one aligned 16-byte block, which is swapped in a single atomic store, so
//     a thread running that code sees either the old bytes or the new ones, never a mix;
//   - our bytes run in memory allocated near the game image (a `jmp rel32` must reach it) and end
//     with a jump back; they make no calls and never touch the stack pointer, so the function's
//     own unwind information stays valid around them.
// The allocated memory is never freed: a render thread may be inside it at the moment of restore.
// For the same reason the destructor does NOT restore: an installed redirect never refers to this
// DLL (its code lives in that separate page), so leaving it in place is always safe, whereas a
// restore from a static destructor could run at any moment of an unload. Only Restore() removes it.
// The page has no unwind data registered: the code makes no calls and leaves rsp unchanged, and it
// can only fault if the register it reads is not what the startup byte check established.

#include <array>
#include <cstdint>
#include <span>

namespace lss::platform {

class CodeRedirect
{
public:
    CodeRedirect() = default;
    ~CodeRedirect() = default;  // leaves an installed redirect in place (see above)

    CodeRedirect(const CodeRedirect&)            = delete;
    CodeRedirect& operator=(const CodeRedirect&) = delete;

    /// Replaces the instructions at `site` (live address; they must be exactly `replaced`, at least
    /// a jump's length, inside one aligned 16-byte block) with a jump to a copy of `code`, followed
    /// by a jump to `resume`. Returns false, changing nothing, if any condition fails.
    ///
    /// The caller must choose `replaced` so that no instruction of the game BEGINS inside it after
    /// its first byte (in practice: exactly one instruction). A thread stopped at such a boundary
    /// when the block is swapped would resume in the middle of the jump. Code runs while the mod is
    /// installed and removed, so that moment is always possible.
    bool Install(std::uintptr_t site, std::span<const std::uint8_t> replaced, std::span<const std::uint8_t> code,
                 std::uintptr_t resume) noexcept;

    /// Puts the original bytes back, if this object replaced them and they are still ours.
    /// Returns false if they were changed after us (they are then left alone).
    bool Restore() noexcept;

    bool IsInstalled() const noexcept { return block_ != 0; }

private:
    using Block = std::array<std::uint8_t, 16>;

    std::uintptr_t block_ = 0;  // the aligned 16-byte block holding the site
    Block          original_{};
    Block          redirected_{};
};

}  // namespace lss::platform
