#pragma once

// Replaces one entry of an engine vtable with a function of our own, and puts it back.
//
// This is the mod's preferred kind of change to the game (the other is a single redirected call, call_redirect.h).
// A vtable entry is data: one aligned 8-byte pointer. Replacing it with an ordinary compiled function needs no trampolines
// or hand-written unwind info, because the compiler emits everything our function needs, and the
// engine calls it exactly as it called the original.
//
// The entry is only replaced if it still holds the function the research identified, so a game
// patch or another mod that already changed it makes the replacement refuse rather than guess.

#include <cstdint>

namespace lss::platform {

class VTableEntry
{
public:
    VTableEntry() = default;
    ~VTableEntry();

    VTableEntry(const VTableEntry&)            = delete;
    VTableEntry& operator=(const VTableEntry&) = delete;

    /// Points the entry at `replacement` if it currently holds `expected`. All addresses are live.
    /// Returns false, changing nothing, if the entry holds anything else or cannot be written.
    /// On success the mod's DLL is pinned for the life of the process (see the .cpp for why).
    bool Replace(std::uintptr_t entryAddress, std::uintptr_t expected, std::uintptr_t replacement) noexcept;

    /// Puts the original function back, if this object replaced it. Returns false if it could
    /// not (the entry no longer holds our function, or cannot be written); the replacement then
    /// stays in place, which is safe because the DLL is pinned.
    bool Restore() noexcept;

    /// True while this object's replacement is in the entry (Restore clears it).
    bool Active() const noexcept { return entry_ != 0; }

    /// The engine's own function, for the replacement to call.
    std::uintptr_t Original() const noexcept { return original_; }

private:
    std::uintptr_t entry_       = 0;
    std::uintptr_t original_    = 0;
    std::uintptr_t replacement_ = 0;
};

}  // namespace lss::platform
