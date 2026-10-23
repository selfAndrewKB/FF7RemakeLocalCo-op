#pragma once

// Edits to a shader's DXBC container for render/ssr_origin: one exact instruction swapped in the shader program, and
// the container's checksum recomputed (D3D12 refuses a container whose checksum is wrong). Pure; tested on the host
// against the game's own SSR shaders (tests/dxbc_fix_test.cpp, tests/data/).
//
// The checksum is the container format's public MD5 variant (as in Wine, vkd3d and DXVK; tools/bloom_shader/d3dc.py):
// MD5 over everything after the magic and the checksum, with its own final block layout.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace lss::render::dxbc_fix {

namespace detail {

inline std::uint32_t Rotate(std::uint32_t value, int bits) noexcept
{
    return (value << bits) | (value >> (32 - bits));
}

inline void Compress(std::array<std::uint32_t, 4>& state, const std::uint8_t* block) noexcept
{
    static constexpr std::uint32_t kK[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr int kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                   5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::uint32_t m[16];
    std::memcpy(m, block, sizeof(m));  // little-endian words (x64)
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; ++i)
    {
        std::uint32_t f = 0;
        int           g = 0;
        if (i < 16)
        {
            f = (b & c) | (~b & d);
            g = i;
        }
        else if (i < 32)
        {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        }
        else if (i < 48)
        {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        }
        else
        {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        f = f + a + kK[i] + m[g];
        a = d;
        d = c;
        c = b;
        b = b + Rotate(f, kS[i]);
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

inline std::uint32_t Word(const std::uint8_t* at) noexcept
{
    std::uint32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

}  // namespace detail

inline constexpr std::size_t kHeaderSize    = 32;  // magic, checksum (16), version, total size, chunk count
inline constexpr std::size_t kChecksumAt    = 4;
inline constexpr std::size_t kChecksumBytes = 16;

/// The container's checksum, over bytes [20, size) (all zero for fewer than 20 bytes, which no container has).
inline std::array<std::uint8_t, kChecksumBytes> Checksum(const std::uint8_t* data, std::size_t size) noexcept
{
    if (size < kChecksumAt + kChecksumBytes)
    {
        return {};
    }
    std::array<std::uint32_t, 4> state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const std::uint8_t* const    body = data + kChecksumAt + kChecksumBytes;
    const std::size_t            n    = size - (kChecksumAt + kChecksumBytes);
    const std::size_t            full = n - n % 64;
    for (std::size_t i = 0; i < full; i += 64)
    {
        detail::Compress(state, body + i);
    }
    const std::size_t   rest = n - full;
    const std::uint32_t bits = static_cast<std::uint32_t>(n * 8);
    const std::uint32_t tail = (bits >> 2) | 1;
    std::uint8_t        block[64]{};
    if (rest >= 56)
    {
        std::memcpy(block, body + full, rest);
        block[rest] = 0x80;
        detail::Compress(state, block);
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, &bits, 4);
        std::memcpy(block + 60, &tail, 4);
    }
    else
    {
        std::memcpy(block, &bits, 4);
        std::memcpy(block + 4, body + full, rest);
        block[4 + rest] = 0x80;
        std::memcpy(block + 60, &tail, 4);
    }
    detail::Compress(state, block);
    std::array<std::uint8_t, kChecksumBytes> out{};
    std::memcpy(out.data(), state.data(), out.size());
    return out;
}

/// Whether `dxbc` is one whole container whose stored checksum is right.
inline bool ChecksumHolds(std::span<const std::uint8_t> dxbc) noexcept
{
    if (dxbc.size() < kHeaderSize || std::memcmp(dxbc.data(), "DXBC", 4) != 0 || detail::Word(dxbc.data() + 24) != dxbc.size())
    {
        return false;
    }
    const auto sum = Checksum(dxbc.data(), dxbc.size());
    return std::memcmp(sum.data(), dxbc.data() + kChecksumAt, sum.size()) == 0;
}

enum class Swap
{
    Done,           // the one instruction was swapped and the checksum recomputed
    NotContainer,   // not a whole DXBC container (magic, size, chunk table)
    BadChecksum,    // the stored checksum is not the one computed here: never re-sign what was not proven first
    NoProgram,     // no SHEX/SHDR chunk, or one that runs past its container
    NotFound,       // the instruction is not in the program
    MoreThanOnce,   // the instruction is there more than once: nothing changed
};

/// Swaps the program's one instruction that equals `from` (token for token, at an instruction start) for `to` (the
/// same number of tokens) and recomputes the checksum, once the container's own checksum has been reproduced (so a
/// shader is re-signed only by a checksum proven on it). Anything but Done leaves `dxbc` untouched.
inline Swap SwapOnce(std::span<std::uint8_t> dxbc, std::span<const std::uint32_t> from,
                     std::span<const std::uint32_t> to) noexcept
{
    if (dxbc.size() < kHeaderSize || std::memcmp(dxbc.data(), "DXBC", 4) != 0 ||
        detail::Word(dxbc.data() + 24) != dxbc.size() || from.empty() || from.size() != to.size())
    {
        return Swap::NotContainer;
    }
    if (!ChecksumHolds(dxbc))
    {
        return Swap::BadChecksum;
    }
    const std::uint32_t chunks = detail::Word(dxbc.data() + 28);
    if (chunks > (dxbc.size() - kHeaderSize) / 4)
    {
        return Swap::NotContainer;
    }
    std::size_t program = 0;  // the first program token's offset
    std::size_t words   = 0;  // the program's length in tokens
    for (std::uint32_t i = 0; i < chunks && program == 0; ++i)
    {
        const std::size_t at = detail::Word(dxbc.data() + kHeaderSize + 4 * i);
        if (at > dxbc.size() || dxbc.size() - at < 16)
        {
            return Swap::NotContainer;
        }
        if (std::memcmp(dxbc.data() + at, "SHEX", 4) != 0 && std::memcmp(dxbc.data() + at, "SHDR", 4) != 0)
        {
            continue;
        }
        const std::size_t size = detail::Word(dxbc.data() + at + 4);
        words                  = detail::Word(dxbc.data() + at + 12);  // token 1: the program's length in tokens
        if (size > dxbc.size() - at - 8 || words < 2 || words * 4 > size)
        {
            return Swap::NoProgram;
        }
        program = at + 8;
    }
    if (program == 0)
    {
        return Swap::NoProgram;
    }
    std::size_t found = 0;
    std::size_t count = 0;
    for (std::size_t token = 2; token < words;)  // tokens 0 and 1: version and length
    {
        const std::uint32_t opcode = detail::Word(dxbc.data() + program + 4 * token);
        std::size_t         length = (opcode >> 24) & 0x7F;
        if ((opcode & 0x7FF) == 0x35)  // customdata: its length is the next token
        {
            length = token + 1 < words ? detail::Word(dxbc.data() + program + 4 * (token + 1)) : 0;
        }
        if (length == 0 || length > words - token)
        {
            return Swap::NoProgram;  // a malformed program: never edit what was not read through
        }
        if (length == from.size() && std::memcmp(dxbc.data() + program + 4 * token, from.data(), 4 * from.size()) == 0)
        {
            found = program + 4 * token;
            ++count;
        }
        token += length;
    }
    if (count == 0)
    {
        return Swap::NotFound;
    }
    if (count > 1)
    {
        return Swap::MoreThanOnce;
    }
    std::memcpy(dxbc.data() + found, to.data(), 4 * to.size());
    const auto sum = Checksum(dxbc.data(), dxbc.size());
    std::memcpy(dxbc.data() + kChecksumAt, sum.data(), sum.size());
    return Swap::Done;
}

// --- Square Enix's SSR pixel shaders (research/RE-third-view-ghost.md §2.2) ---------------------------------------
// The pass draws every view over the whole buffer, so SV_Position (v1) is already the absolute pixel, yet the shader's
// first instruction adds the view's corner (View.ViewRectMin, cb1[121]) again before its GBuffer and depth loads: a view
// one column in reads the next column. The fix loads at the pixel itself (the same at corner 0, so right for every view).

/// add r0.xy, v1.xyxx, cb1[121].xyxx
inline constexpr std::array<std::uint32_t, 8> kSsrDoubledCorner{0x08000000, 0x00100032, 0x00000000, 0x00101046,
                                                                0x00000001, 0x00208046, 0x00000001, 0x00000079};
/// mov r0.xy, v1.xyxx; nop; nop; nop (the same length, so every later offset in the program stays)
inline constexpr std::array<std::uint32_t, 8> kSsrPixelItself{0x05000036, 0x00100032, 0x00000000, 0x00101046,
                                                              0x00000001, 0x0100003A, 0x0100003A, 0x0100003A};

inline Swap CorrectSsr(std::span<std::uint8_t> dxbc) noexcept
{
    return SwapOnce(dxbc, kSsrDoubledCorner, kSsrPixelItself);
}

}  // namespace lss::render::dxbc_fix
