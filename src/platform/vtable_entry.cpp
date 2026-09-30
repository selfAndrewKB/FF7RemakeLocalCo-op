#include "platform/vtable_entry.h"

#include "platform/module_pin.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::platform {

namespace {

/// Swaps the pointer at `entry` from `expected` to `desired` in one atomic store, lifting the
/// page's read-only protection only for that store. Returns false if the entry did not hold
/// `expected` (nothing is written then) or its protection could not be changed.
bool SwapPointer(std::uintptr_t entry, std::uintptr_t expected, std::uintptr_t desired) noexcept
{
    auto* slot = reinterpret_cast<void**>(entry);

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        return false;
    }

    // Compare-and-swap: the engine may call through this entry on another thread at any moment,
    // and it must always read either the old pointer or the new one, never a torn value.
    const void* previous = InterlockedCompareExchangePointer(
        slot, reinterpret_cast<void*>(desired), reinterpret_cast<void*>(expected));

    // If putting the old protection back fails, the page just stays writable. That is harmless
    // to the game, and not a reason to report a swap that did happen as failed.
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return previous == reinterpret_cast<void*>(expected);
}

}  // namespace

VTableEntry::~VTableEntry()
{
    // A no-op if the owner already restored (or chose not to); kept so a forgotten restore
    // cannot outlive the object.
    Restore();
}

bool VTableEntry::Replace(std::uintptr_t entryAddress, std::uintptr_t expected, std::uintptr_t replacement) noexcept
{
    const bool aligned = entryAddress % sizeof(void*) == 0;  // the atomic store requires it
    if (entry_ != 0 || !aligned || !PinThisModule() || !SwapPointer(entryAddress, expected, replacement))
    {
        return false;
    }
    entry_       = entryAddress;
    original_    = expected;
    replacement_ = replacement;
    return true;
}

bool VTableEntry::Restore() noexcept
{
    if (entry_ == 0)
    {
        return true;
    }
    // Only restores if the entry still holds our function (the compare-and-swap refuses
    // otherwise): if something replaced it after us, overwriting that would break the other party.
    // On failure the entry stays recorded as ours; the pinned DLL keeps our function valid.
    if (!SwapPointer(entry_, replacement_, original_))
    {
        return false;
    }
    entry_ = 0;
    return true;
}

}  // namespace lss::platform
