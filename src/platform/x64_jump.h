#pragma once

// Encoding of the one x64 instruction the mod writes into game code: `jmp rel32` (E9 + a signed
// 32-bit displacement measured from the end of the 5-byte instruction).

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace lss::platform {

inline constexpr std::size_t kJumpLength = 5;

/// `jmp qword ptr [rip+0]`, followed in the placed code by the 8-byte address it jumps to (any distance).
inline constexpr std::uint8_t kAbsoluteJump[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};

/// The bytes of a `jmp` placed at `from` that lands on `to`, or empty if `to` is out of the
/// instruction's ±2 GiB reach.
constexpr std::optional<std::array<std::uint8_t, kJumpLength>> EncodeJump(std::uintptr_t from, std::uintptr_t to) noexcept
{
    const auto displacement = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from + kJumpLength);
    if (displacement < std::numeric_limits<std::int32_t>::min() || displacement > std::numeric_limits<std::int32_t>::max())
    {
        return std::nullopt;
    }
    const auto rel = static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
    return std::array<std::uint8_t, kJumpLength>{
        0xE9,
        static_cast<std::uint8_t>(rel),
        static_cast<std::uint8_t>(rel >> 8),
        static_cast<std::uint8_t>(rel >> 16),
        static_cast<std::uint8_t>(rel >> 24),
    };
}

}  // namespace lss::platform
