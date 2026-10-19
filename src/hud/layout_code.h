#pragma once

// The machine code for the two 16:9 assumptions in Square's HUD placement that misplace things in a split half's band
// (RE-hud-overlay-pass.md §6.2, §6.3; Rebirth's hud/centered_frame_code.h and hud/arrow_edge_code.h, re-found here).
// Each is installed with platform::CodeRedirect over ONE game instruction inside one aligned 16-byte block, reads a
// flag byte the HUD layout sets while the band applies (hud/overlay_layout.h), and with the flag clear does exactly
// what the game does. No calls, no stack writes, rsp unchanged. Host-tested by executing them (tests/layout_code_test.cpp).
//
// 1. The centred 1920x1080 frame. EndUI__GetCentered1920x1080Frame(FBox2D* out) (0x1413488A0, 14 callers: the
//    off-screen search arrow, battle text, the item-get log...) returns a 1920x1080 box centred in the layout
//    L = UI size / UI scale. For a 16:9 image (single player) that is the whole layout; in a 32:9 band it is a strip
//    in the middle. While the band applies, the box is made the whole layout, as a 16:9 image makes it.
//    Replaced: `movss [rbx+0Ch], xmm7` (Max.Y, 5 bytes at 0x141348924). There rbx = out, xmm2 = Min.X, xmm1 = Min.Y
//    (already stored), xmm3 = Max.X (stored by the function's last instruction, after this one), xmm7 = Max.Y
//    (restored from the stack by the next instruction), rcx free (last read by the call before), rax = the return
//    value (untouched). Flag set: Min = (0, 0), xmm3 += Min.X (= L.X), xmm7 += Min.Y (= L.Y); then the store.
//
// 2. The off-screen arrow's edge. EndUI__PlaceMarkerAtWorld_EdgeClamped60_120 (0x1414504B0) pins an arrow to a side
//    edge when its angle from the vertical is between 60° and 120°, else to the top or bottom: a 16:9 rectangle's
//    corners. While the band applies, the edge is the one the ray from the centre really leaves by: a side edge
//    exactly when the side edge's y (ySide, already computed) lies within [0, L.H - h], unless the ray is vertical.
//    Replaced: `comiss xmm0, [60.0]` (7 bytes at 0x141450703). There xmm0 = |angle|, xmm7 = 0, xmm8 = L.H - h,
//    xmm10 = ySide; rcx is free (both branches set it before reading). The code also runs the three instructions
//    that follow it in the game (two stack loads and an add, which leave the flags alone), then jumps to the side
//    branch (0x141450726) or the top/bottom branch (0x14145075E). Flag clear: the game's own two compares, against
//    copies of its 60.0 and 120.0 (read from the game at install: the code pool may lie beyond rel32 reach of them).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::hud::layout_code {

// --- 1. The centred frame ---------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kFrameFunction = 0x1413488A0;
inline constexpr std::uintptr_t kFrameSite     = 0x141348924;
inline constexpr std::uintptr_t kFrameResume   = 0x141348929;

/// The function from its entry through the replaced store, as the research read it.
inline constexpr std::uint8_t kFrameBytes[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x0F, 0x29, 0x7C, 0x24, 0x20, 0x48, 0x8D, 0x4C, 0x24, 0x40,
    0xE8, 0x08, 0xFF, 0xFF, 0xFF, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x40, 0xF3, 0x0F, 0x10, 0x7C, 0x24, 0x44, 0x0F, 0x14,
    0xC7, 0x66, 0x48, 0x0F, 0x7E, 0xC1, 0xE8, 0xCF, 0x0B, 0x06, 0x00, 0xF3, 0x0F, 0x10, 0x2D, 0xE7, 0x95, 0xC4, 0x03,
    0x48, 0x8B, 0xC3, 0xF3, 0x0F, 0x5E, 0xE8, 0xF3, 0x0F, 0x59, 0xFD, 0x0F, 0x28, 0xDD, 0xF3, 0x0F, 0x59, 0x5C, 0x24,
    0x40, 0x0F, 0x28, 0xCF, 0xF3, 0x0F, 0x5C, 0x0D, 0x38, 0x9F, 0xC4, 0x03, 0x0F, 0x28, 0xD3, 0xF3, 0x0F, 0x5C, 0x15,
    0x95, 0x9F, 0xC4, 0x03, 0xF3, 0x0F, 0x59, 0x0D, 0x11, 0x94, 0xC4, 0x03, 0xF3, 0x0F, 0x59, 0x15, 0x09, 0x94, 0xC4,
    0x03, 0xF3, 0x0F, 0x5C, 0xF9, 0xF3, 0x0F, 0x11, 0x4B, 0x04, 0xF3, 0x0F, 0x5C, 0xDA, 0xF3, 0x0F, 0x11, 0x13, 0xF3,
    0x0F, 0x11, 0x7B, 0x0C};
static_assert(sizeof(kFrameBytes) == kFrameResume - kFrameFunction);

/// The instruction replaced: movss [rbx+0Ch], xmm7.
inline constexpr std::uint8_t kFrameStore[] = {0xF3, 0x0F, 0x11, 0x7B, 0x0C};

inline constexpr std::size_t kFrameCodeSize = 41;

/// `band` is a byte: nonzero while the HUD is laid out in a split half's band.
inline std::array<std::uint8_t, kFrameCodeSize> FrameCode(const std::uint8_t& band) noexcept
{
    std::array<std::uint8_t, kFrameCodeSize> code = {
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,        //  0: mov   rcx, &band
        0x80, 0x39, 0x00,                          // 10: cmp   byte [rcx], 0
        0x74, 0x15,                                // 13: je    store (36)
        0xF3, 0x0F, 0x58, 0xF9,                    // 15: addss xmm7, xmm1          Max.Y = L.Y
        0xF3, 0x0F, 0x58, 0xDA,                    // 19: addss xmm3, xmm2          Max.X = L.X (stored by the game next)
        0xC7, 0x03, 0x00, 0x00, 0x00, 0x00,        // 23: mov   dword [rbx], 0      Min.X = 0
        0xC7, 0x43, 0x04, 0x00, 0x00, 0x00, 0x00,  // 29: mov   dword [rbx+4], 0    Min.Y = 0
        0xF3, 0x0F, 0x11, 0x7B, 0x0C,              // 36: store: movss [rbx+0Ch], xmm7 (the replaced instruction)
    };                                             // 41: (the jump back to 0x141348929 is appended here)
    const auto address = reinterpret_cast<std::uint64_t>(&band);
    std::memcpy(&code[2], &address, sizeof(address));
    return code;
}

// --- 2. The arrow's edge ----------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kArrowCheckStart = 0x1414506C6;  // checked from here...
inline constexpr std::uintptr_t kArrowSite       = 0x141450703;
inline constexpr std::uintptr_t kArrowResume     = 0x14145070A;  // never reached: the code always jumps to a branch
inline constexpr std::uintptr_t kArrowSide       = 0x141450726;  // the side branch
inline constexpr std::uintptr_t kArrowTopBottom  = 0x14145075E;  // the top/bottom branch (...through its first byte)
inline constexpr std::uintptr_t kArrowSixty      = 0x144F92520;  // 60.0f
inline constexpr std::uintptr_t kArrowOneTwenty  = 0x144F925D0;  // 120.0f

/// 0x1414506C6 through 0x141450761: the atan2 and its scaling, the split, the side branch's start and the top/bottom
/// branch's first instruction, as the research read them.
inline constexpr std::uint8_t kArrowBytes[] = {
    0x41, 0x0F, 0x28, 0xCA, 0x41, 0x0F, 0x28, 0xC1, 0xE8, 0x9D, 0x39, 0x7C, 0x00, 0x0F, 0x28, 0xC8, 0xF3, 0x45, 0x0F,
    0x59, 0xD7, 0xF3, 0x0F, 0x59, 0x0D, 0x25, 0x1E, 0xB4, 0x03, 0x44, 0x0F, 0x28, 0x7C, 0x24, 0x40, 0xF3, 0x45, 0x0F,
    0x58, 0xD5, 0x44, 0x0F, 0x28, 0x6C, 0x24, 0x60, 0xF3, 0x45, 0x0F, 0x59, 0xCE, 0x0F, 0x28, 0xC1, 0x0F, 0x54, 0x05,
    0x9D, 0x48, 0xB4, 0x03, 0x0F, 0x2F, 0x05, 0x16, 0x1E, 0xB4, 0x03, 0x44, 0x0F, 0x28, 0x74, 0x24, 0x50, 0xF3, 0x45,
    0x0F, 0x58, 0xCC, 0x44, 0x0F, 0x28, 0x64, 0x24, 0x70, 0x76, 0x41, 0x0F, 0x2F, 0x05, 0xAC, 0x1E, 0xB4, 0x03, 0x73,
    0x38, 0x0F, 0x2F, 0xCF, 0x73, 0x0A, 0xF3, 0x0F, 0x10, 0x05, 0x5D, 0x1E, 0xB4, 0x03, 0xEB, 0x08, 0xF3, 0x0F, 0x10,
    0x05, 0x83, 0x25, 0xB4, 0x03, 0x73, 0x0F, 0xF3, 0x0F, 0x11, 0x7C, 0x24, 0x20, 0xF3, 0x44, 0x0F, 0x11, 0x54, 0x24,
    0x24, 0xEB, 0x3A, 0xF3, 0x44, 0x0F, 0x11, 0x5C, 0x24, 0x20, 0xF3, 0x44, 0x0F, 0x11, 0x54, 0x24, 0x24, 0xEB, 0x2A,
    0x0F, 0x2F, 0xF7, 0x73, 0x0A};
static_assert(sizeof(kArrowBytes) == 0x141450763 - kArrowCheckStart);

/// The instruction replaced: comiss xmm0, [60.0].
inline constexpr std::uint8_t kArrowCompare[] = {0x0F, 0x2F, 0x05, 0x16, 0x1E, 0xB4, 0x03};

inline constexpr std::size_t kArrowCodeSize = 143;
inline constexpr float       kVerticalBelow = 0.001f;    // |angle| below this, or above the next: a vertical ray
inline constexpr float       kVerticalAbove = 179.999f;

/// Live addresses: the band flag and the two branches; the game's 60.0 and 120.0 by value.
inline std::array<std::uint8_t, kArrowCodeSize> ArrowCode(const std::uint8_t& band, std::uintptr_t side,
                                                          std::uintptr_t topBottom, float sixty, float oneTwenty) noexcept
{
    std::array<std::uint8_t, kArrowCodeSize> code = {
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,  //   0: mov    rcx, &band
        0x80, 0x39, 0x00,                    //  10: cmp    byte [rcx], 0
        0x74, 0x4B,                          //  13: je     game (90)
        0x44, 0x0F, 0x28, 0x74, 0x24, 0x50,  //  15: movaps xmm14, [rsp+50h]   (the game's next three instructions)
        0xF3, 0x45, 0x0F, 0x58, 0xCC,        //  21: addss  xmm9, xmm12
        0x44, 0x0F, 0x28, 0x64, 0x24, 0x70,  //  26: movaps xmm12, [rsp+70h]
        0x0F, 0x2F, 0x05, 88, 0, 0, 0,       //  32: comiss xmm0, [rip+vertical below] (127)
        0x72, 0x23,                          //  39: jb     topBottom (76)       straight up or down
        0x0F, 0x2F, 0x05, 83, 0, 0, 0,       //  41: comiss xmm0, [rip+vertical above] (131)
        0x77, 0x1A,                          //  48: ja     topBottom (76)
        0x44, 0x0F, 0x2F, 0xD7,              //  50: comiss xmm10, xmm7        ySide < 0: leaves by the top
        0x72, 0x14,                          //  54: jb     topBottom (76)
        0x45, 0x0F, 0x2F, 0xD0,              //  56: comiss xmm10, xmm8        ySide > L.H - h: leaves by the bottom
        0x77, 0x0E,                          //  60: ja     topBottom (76)
        0xFF, 0x25, 0, 0, 0, 0,              //  62: side: jmp [rip+0]
        0, 0, 0, 0, 0, 0, 0, 0,              //  68:   the side branch
        0xFF, 0x25, 0, 0, 0, 0,              //  76: topBottom: jmp [rip+0]
        0, 0, 0, 0, 0, 0, 0, 0,              //  82:   the top/bottom branch
        0x0F, 0x2F, 0x05, 38, 0, 0, 0,       //  90: game: comiss xmm0, [rip+sixty] (135)
        0x44, 0x0F, 0x28, 0x74, 0x24, 0x50,  //  97: movaps xmm14, [rsp+50h]
        0xF3, 0x45, 0x0F, 0x58, 0xCC,        // 103: addss  xmm9, xmm12
        0x44, 0x0F, 0x28, 0x64, 0x24, 0x70,  // 108: movaps xmm12, [rsp+70h]
        0x76, 0xD8,                          // 114: jbe    topBottom (76)
        0x0F, 0x2F, 0x05, 16, 0, 0, 0,       // 116: comiss xmm0, [rip+oneTwenty] (139)
        0x73, 0xCF,                          // 123: jae    topBottom (76)
        0xEB, 0xBF,                          // 125: jmp    side (62)
        0, 0, 0, 0,                          // 127: vertical below
        0, 0, 0, 0,                          // 131: vertical above
        0, 0, 0, 0,                          // 135: sixty
        0, 0, 0, 0,                          // 139: oneTwenty
    };                                       // 143: (the appended jump back is never reached)
    const auto put = [&code](std::size_t at, const void* value, std::size_t size) { std::memcpy(&code[at], value, size); };
    const auto flag = reinterpret_cast<std::uint64_t>(&band);
    put(2, &flag, sizeof(flag));
    put(68, &side, sizeof(side));
    put(82, &topBottom, sizeof(topBottom));
    put(127, &kVerticalBelow, sizeof(float));
    put(131, &kVerticalAbove, sizeof(float));
    put(135, &sixty, sizeof(float));
    put(139, &oneTwenty, sizeof(float));
    return code;
}

}  // namespace lss::hud::layout_code
