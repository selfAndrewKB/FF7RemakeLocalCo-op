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

/// A code site a module depends on: its IDA address and the instruction bytes there.
struct CodeSite
{
    const wchar_t*                what;
    std::uintptr_t                address;
    std::span<const std::uint8_t> bytes;
};

/// The first of `sites` whose bytes differ in the image, or null when every one is as listed.
const CodeSite* FirstDiffering(const GameImage& image, std::span<const CodeSite> sites) noexcept;

/// The live address of `fact`'s function, or empty (logged) if its entry bytes differ at three reads a millisecond
/// apart (a loader can hook a function for a moment while it starts).
std::optional<std::uintptr_t> FindFunction(const GameImage& image, const FunctionFact& fact) noexcept;

/// As FindFunction, for a function the mod only calls and never patches: another hook's jump at its entry (to a place
/// outside the game's image) is also accepted when the expected bytes resume after it and at least six of them run
/// to the end of the entry (logged), since a call through the hook reaches the game's function. Never for a function
/// the mod redirects: that would write over the other hook.
std::optional<std::uintptr_t> FindCallable(const GameImage& image, const FunctionFact& fact) noexcept;

/// Points `fact`'s slot at `replacement` if the slot holds the expected function and that function has the expected
/// entry bytes, or another hook's jump before them as FindCallable allows (the slot replacement never touches the
/// entry). Returns false (logged), changing nothing, otherwise.
bool ReplaceSlot(const GameImage& image, const SlotFact& fact, std::uintptr_t replacement, VTableEntry& entry) noexcept;

}  // namespace lss::platform
