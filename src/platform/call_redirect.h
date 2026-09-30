#pragma once

// Sends one `call rel32` (or one tail call, `jmp rel32`) in the game's code to a function of ours
// instead of its usual target, and puts it back. Every other caller of that target is untouched.
//
// This is for a decision the game makes by calling a function directly, where the mod must
// change the result for one caller only. The replacement is an ordinary compiled function with
// the target's signature, which may call the target itself (Original()). The change is kept as
// small as a redirect can be:
//   - the site must hold `E8 rel32` resolving to the research's target before anything is written;
//   - only the 4-byte displacement changes, inside one aligned 16-byte block swapped atomically, so
//     a thread executing the call reaches either the old target or the new one (a call crossing two
//     blocks is changed only from the one thread that runs it: InstallFromOwnThread);
//   - the displacement points at a thunk in memory allocated near the game image, which is a single
//     `jmp [rip+0]` to the replacement. The thunk never touches the stack, so the call arrives in
//     the replacement exactly as it would in the target, return address included, and the
//     replacement's own unwind information covers it.
// The thunk needs no unwind data of its own: it is a single jump that touches neither the stack
// nor any register the call relies on. The thunk jumps into our DLL, so Install pins the DLL
// (platform/module_pin.h). The thunk page is never freed and the destructor does not restore;
// only Restore() removes the redirect.

#include "platform/near_code.h"

#include <cstdint>

namespace lss::platform {

class CallRedirect
{
public:
    CallRedirect() = default;
    ~CallRedirect() = default;  // leaves an installed redirect in place (see above)

    CallRedirect(const CallRedirect&)            = delete;
    CallRedirect& operator=(const CallRedirect&) = delete;

    /// Points the `call` at `site` (live address) at `replacement`, if it is a `call rel32` to
    /// `target` whose five bytes lie inside one aligned 16-byte block. Returns false, changing
    /// nothing, if any condition fails or this object already holds a redirect.
    bool Install(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement) noexcept;

    /// The same for a tail call, `jmp rel32` (E9) to `target`, the caller's frame already torn down.
    /// The replacement is an ordinary function with the target's signature: its `ret` goes to the
    /// caller's caller, exactly where the target's would. (It must not tail-jump to the target itself
    /// if it has work to do after it.)
    bool InstallTailJump(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement) noexcept;

    /// The same for a call whose five bytes cross into the next aligned block. The two blocks are
    /// swapped one after the other, each atomically, so a thread running the call between the two
    /// swaps would take a mixed displacement. Call it only from the one thread that can run the site,
    /// from outside the function that holds it (so that thread is not at the call); Restore must then
    /// be called under the same condition. A site inside one block is installed as by Install.
    bool InstallFromOwnThread(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement) noexcept;

    /// Puts the original displacement back, if this object changed it and it is still ours.
    /// Returns false if it was changed after us (it is then left alone).
    bool Restore() noexcept;

    bool IsInstalled() const noexcept { return block_ != 0; }

    /// The call's usual target, for the replacement to call.
    std::uintptr_t Original() const noexcept { return target_; }

private:
    bool Place(std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement, bool crossingAllowed,
               std::uint8_t opcode) noexcept;

    std::uintptr_t block_  = 0;  // the aligned 16-byte block holding the call (its first byte)
    std::uintptr_t target_ = 0;
    CodeBlock      original_{};
    CodeBlock      redirected_{};
    bool           crosses_ = false;  // the call's last bytes are in the next block
    CodeBlock      nextOriginal_{};
    CodeBlock      nextRedirected_{};
};

}  // namespace lss::platform
