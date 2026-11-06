#pragma once

// The game's executable image in memory, and the translation from IDA addresses to live ones and back (the IDA
// form of a live address, for log lines).
//
// Every engine address in this mod is written as it appears in IDA (image base 0x140000000),
// because that is how the research documents cite them. The game is loaded at a different base
// each run (ASLR), so each address is rebased once, here, and checked to lie inside the image.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace lss::platform {

/// The game's executable, as its process and its module are named.
inline constexpr const wchar_t* kGameExe = L"ff7remake_.exe";

class GameImage
{
public:
    /// Finds the running game image. Empty if the process is not FF7 Remake.
    static std::optional<GameImage> Locate() noexcept;

    /// The live address of an IDA address, or empty if it falls outside the image.
    std::optional<std::uintptr_t> Rebase(std::uintptr_t idaAddress, std::size_t length = 1) const noexcept;

    /// True if the bytes at an IDA address are exactly `expected`. This is how the mod confirms
    /// that the game build is the one the research was done on before relying on any address.
    bool BytesMatch(std::uintptr_t idaAddress, std::span<const std::uint8_t> expected) const noexcept;

    /// The pointer stored at an IDA address (for vtable entries), or empty if outside the image.
    std::optional<std::uintptr_t> ReadPointer(std::uintptr_t idaAddress) const noexcept;


private:
    GameImage(std::uintptr_t base, std::size_t size) noexcept : base_(base), size_(size) {}

    std::uintptr_t base_;
    std::size_t    size_;
};

/// The IDA address of a live address of the image Locate last found (the research's and the log's form): for a
/// module's log line after install, when no GameImage is at hand. 0 before Locate; an address outside the image
/// gives no meaningful value.
std::uintptr_t IdaAddress(std::uintptr_t live) noexcept;

}  // namespace lss::platform
