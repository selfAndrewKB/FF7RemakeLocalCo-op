#pragma once

// Finding engine functions and vtable slots only where the research says they are.
//
// A function is used only if its first bytes are exactly the ones read in IDA, and a vtable slot is
// replaced only if it holds exactly the function the research named. Anything else means a game
// patch or another mod changed it, and the caller stands the feature aside instead of guessing.
// Every check that fails is logged with what it was for.

#include "platform/image.h"
#include "platform/vtable_entry.h"

#include <cstdint>
#include <optional>
#include <span>

namespace lss::platform {

/// An engine function: its IDA address and the bytes at its entry. Call and RIP-relative operands
/// are displacements within the image, so they read the same at any load address; only absolute
/// 64-bit addresses would be relocated, and none of the listed entries contains one.
struct FunctionFact
{
    const wchar_t*                what;
    std::uintptr_t                address;
    std::span<const std::uint8_t> entryBytes;
};

/// A vtable slot and the function it must hold.
struct SlotFact
{
    const wchar_t* what;
    std::uintptr_t slot;
    FunctionFact   target;
};

/// The live address of `fact`'s function, or empty (logged) if its entry bytes differ.
std::optional<std::uintptr_t> FindFunction(const GameImage& image, const FunctionFact& fact) noexcept;

/// Points `fact`'s slot at `replacement` if the slot holds the expected function and that function
/// has the expected entry bytes. Returns false (logged), changing nothing, otherwise.
bool ReplaceSlot(const GameImage& image, const SlotFact& fact, std::uintptr_t replacement, VTableEntry& entry) noexcept;

}  // namespace lss::platform
