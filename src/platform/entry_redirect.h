#pragma once

// Sends every call of one game function through a function of ours, which may call the game's
// function itself (Original()), and puts it back.
//
// This is for a game function the mod must follow at every caller: a setter with many call sites,
// some of which cannot take a call_redirect.h because their five bytes cross a 16-byte block. The
// change is kept as small as the call redirect's:
//   - the function's first instruction must be exactly the bytes the research read, one instruction
//     of at least a jump's length, position-independent (no RIP-relative operand, no branch), inside
//     one aligned 16-byte block, and no branch in the game may land inside it after its first byte;
//   - that instruction is replaced by `jmp rel32` in one atomic 16-byte store, so a thread entering
//     the function reaches either the game's code or ours, never a mix;
//   - the jump lands on a thunk near the game image, a single `jmp [rip+0]` to the replacement, which
//     never touches the stack: the replacement is entered exactly as the game function would be,
//     return address included, and its own unwind information covers it;
//   - Original() is a trampoline beside the thunk: the replaced instruction, then a jump to the
//     instruction after it. The function then runs from where it would have been had nothing
//     changed, so its own unwind information stays valid.
// The trampoline has no unwind data of its own: it runs one position-independent instruction (at
// the function's entry, a register save into the caller's home area) and a jump. The page is never
// freed, the DLL is pinned (the thunk jumps into it), and the destructor does not restore; only
// Restore() removes the redirect.

#include "platform/code_memory.h"

#include <cstdint>
#include <span>

namespace lss::platform {

class EntryRedirect
{
public:
    /// The most an entry redirect copies: a home-area store or `sub rsp, imm32` (up to 7), or the five pushes
    /// that open the game's input dispatcher (8).
    static constexpr std::size_t kMaxFirstBytes = 8;

    EntryRedirect() = default;
    ~EntryRedirect() = default;  // leaves an installed redirect in place (see above)

    EntryRedirect(const EntryRedirect&)            = delete;
    EntryRedirect& operator=(const EntryRedirect&) = delete;

    /// Sends calls of the function at `entry` (live address) to `replacement`, if its first
    /// instruction is exactly `first` (5 to kMaxFirstBytes bytes, inside one aligned 16-byte block). The
    /// caller guarantees `first` is one position-independent instruction no branch lands inside (or a run
    /// of such instructions, when only the thread that installs the redirect ever runs the function, and it
    /// installs from outside it). Pass the instruction's bytes, never a longer entry check: a trampoline
    /// replaying a cut instruction corrupts the stack (live 2026-10-19). Returns false, changing nothing, if
    /// any condition fails or this object already holds a redirect.
    bool Install(std::uintptr_t entry, std::span<const std::uint8_t> first, std::uintptr_t replacement) noexcept;

    /// The same redirect placed at the function's second instruction, for a first instruction that
    /// crosses a 16-byte block: `first` (any length up to 15) and `second` (5 to 16 bytes, inside one
    /// block) must be exactly the function's first two instructions, both position-independent. The
    /// jump replaces `second`; the trampoline runs `first`, `second`, then jumps to the third.
    /// Equivalent to an entry redirect only when the caller has checked, for this function, that
    /// `first` is free of effects the replacement could see (a store into the caller's home area: no
    /// rsp change, no argument register written, its slot read by nothing before the trampoline
    /// stores it again) and that no branch in the game lands on `second` (live 2026-10-07: 1.005's
    /// FieldActionMgr_AddPoint, research/RE-1005-field-2.md §5). Unlike an entry redirect, `first`
    /// runs on every call before the replacement does, also on calls the replacement does not pass
    /// on to Original(): a replacement that skips the game's function must not mind that store.
    bool InstallAtSecondInstruction(std::uintptr_t entry, std::span<const std::uint8_t> first,
                                    std::span<const std::uint8_t> second, std::uintptr_t replacement) noexcept;

    /// Puts the original instruction back, if this object replaced it and it is still ours.
    /// Returns false if it was changed after us (it is then left alone).
    bool Restore() noexcept;

    bool IsInstalled() const noexcept { return block_ != 0; }

    /// The game's function, for the replacement to call (0 before Install; kept after Restore: a
    /// call already inside the replacement may still use it).
    std::uintptr_t Original() const noexcept { return trampoline_; }

private:
    bool Place(std::uintptr_t entry, std::span<const std::uint8_t> copied, std::size_t siteOffset,
               std::uintptr_t replacement) noexcept;

    std::uintptr_t block_       = 0;  // the aligned 16-byte block holding the replaced instruction
    std::uintptr_t trampoline_  = 0;  // the replaced instruction(s), then a jump back
    CodeBlock      original_{};
    CodeBlock      redirected_{};
};

}  // namespace lss::platform
