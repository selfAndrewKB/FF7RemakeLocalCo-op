#pragma once

// The machine code for the 16:9 assumptions in Square's HUD placement that misplace things in a split half's band
// (research/RE-hud-overlay-pass.md).
// Each is installed with platform::CodeRedirect inside one aligned 16-byte block. Pieces 1 to 3 replace ONE game instruction,
// read a flag byte the HUD layout sets while the band applies (hud/overlay_layout.h), and with the flag clear do exactly what
// the game does; piece 4 replaces two and needs no flag (below). No calls, no stack writes, rsp unchanged. Host-tested by
// executing them (tests/layout_code_test.cpp).
//
// 1. The centred 1920x1080 frame. EndUI__GetCentered1920x1080Frame(FBox2D* out) (kFrameFunction, 14 callers: the
//    off-screen search arrow, battle text, the item-get log...) returns a 1920x1080 box centred in the layout
//    L = UI size / UI scale. For a 16:9 image (single player) that is the whole layout; in a 32:9 band it is a strip
//    in the middle. While the band applies, the box is made the whole layout, as a 16:9 image makes it.
//    Replaced: `movss [rbx+0Ch], xmm7` (Max.Y, 5 bytes at kFrameSite). There rbx = out, xmm2 = Min.X, xmm1 = Min.Y
//    (already stored), xmm3 = Max.X (stored by the function's last instruction, after this one), xmm7 = Max.Y
//    (restored from the stack by the next instruction), rcx free (last read by the call before), rax = the return
//    value (untouched). Flag set: Min = (0, 0), xmm3 += Min.X (= L.X), xmm7 += Min.Y (= L.Y); then the store.
//
// 2. The off-screen arrow's edge. EndUI__PlaceMarkerAtWorld_EdgeClamped60_120 pins an arrow to a side
//    edge when its angle from the vertical is between 60° and 120°, else to the top or bottom: a 16:9 rectangle's
//    corners. While the band applies, the edge is the one the ray from the centre really leaves by: a side edge
//    exactly when the side edge's y (ySide, already computed) lies within [0, L.H - h], unless the ray is vertical.
//    Replaced: `comiss xmm0, [60.0]` (7 bytes at kArrowSite). There xmm0 = |angle|, xmm7 = 0, xmm8 = L.H - h,
//    xmm10 = ySide; rcx is free (both branches set it before reading). The code also runs the three instructions
//    that follow it in the game (two stack loads and an add, which leave the flags alone), then jumps to the side
//    branch (kArrowSide) or the top/bottom branch (kArrowTopBottom). Flag clear: the game's own two compares, against
//    copies of its 60.0 and 120.0 (read from the game at install: the code pool may lie beyond rel32 reach of them).
//
// 3. The battle's off-screen enemy arrows, the same split in their own placement (EndUI_BattleOutsideIcon_
//    PlaceEdgeClamped60_120; research/RE-outside-icons-per-view.md): the same rule while the band applies.
//    Replaced: `addss xmm12, [rsp+20h]` (7 bytes at kBattleArrowSite), which makes ySide; the flags of the game's 60°
//    compare are live across it. There xmm1 = |angle|, xmm6 = 0, xmm11 = L.H - h, [rsp+20h] = the centre's y; rcx is
//    free (set by the atan2f call before, next written at kBattleArrowSite + 0x6A, read by neither branch). Flag set:
//    the side branch (kBattleArrowSide) or the top/bottom branch (kBattleArrowTopBottom), each of which sets its own
//    flags. Flag clear: the add, the 60° compare again (the flag test destroyed its flags), then the game's own jbe
//    and 120° compare.
//
// 4. The overlay's layout scale (research/RE-hud-overlay-pass.md). The render lays the
//    overlay out at height/1080 of its rect, while every conversion from UI pixels to layout units uses the contain scale of
//    the same rect, height/1080 for 16:9 and wider and width/1920 for narrower. The game only makes 16:9 images, where the two
//    are equal. A view narrower than 16:9 lays out in a band narrower than 16:9 and the two part: world markers land off
//    their objects, by the ratio of the two scales, and what the game keeps in its centred frame falls off the view. The
//    scale becomes the contain rule, min(height/1080, width/1920), with the game's own constants; a rect of 16:9 or wider
//    gets exactly the game's value (the stored 1/1920 rounds up and the stored 1/1080 down, so the width's term is never the
//    smaller there), so there is no flag. The `mulss` crosses a block, so the two instructions before it (kScaleSite)
//    are replaced by themselves and the rule, resuming after it, its bytes left in place unrun. There edx = the rect's width,
//    xmm2 = float(height), and xmm5 is free (next written at kScaleSite + 0x261, not read before). A thread stopped
//    between the two replaced instructions would resume inside the jump, so this is installed from the game thread
//    outside the render, the only thread that runs it.

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
    };                                             // 41: (the jump back to kFrameResume is appended here)
    const auto address = reinterpret_cast<std::uint64_t>(&band);
    std::memcpy(&code[2], &address, sizeof(address));
    return code;
}

// --- 2. The arrow's edge ----------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kArrowCheckStart = 0x1414506C6;  // checked from here...
inline constexpr std::uintptr_t kArrowSite       = 0x141450703;
inline constexpr std::uintptr_t kArrowResume     = 0x14145070A;  // never reached: the code always jumps to a branch
inline constexpr std::uintptr_t kArrowSide       = 0x141450726;  // the side branch
inline constexpr std::uintptr_t kArrowTopBottom  = 0x14145075E;  // the top/bottom branch (checked through its first instruction)
inline constexpr std::uintptr_t kArrowSixty      = 0x144F92520;  // 60.0f
inline constexpr std::uintptr_t kArrowOneTwenty  = 0x144F925D0;  // 120.0f

/// From kArrowCheckStart: the atan2 and its scaling, the split, the side branch's start and the top/bottom
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

// --- 3. The battle arrows' edge -------------------------------------------------------------------------------
inline constexpr std::uintptr_t kBattleArrowCheckStart = 0x1413E3938;  // checked from here...
inline constexpr std::uintptr_t kBattleArrowSite       = 0x1413E3977;
inline constexpr std::uintptr_t kBattleArrowResume     = 0x1413E397E;  // the game's jbe (top/bottom at 60° or less)
inline constexpr std::uintptr_t kBattleArrowSide       = 0x1413E3989;  // the side branch
inline constexpr std::uintptr_t kBattleArrowTopBottom  = 0x1413E39B9;  // the top/bottom branch (checked through its first instruction)

/// From kBattleArrowCheckStart: the atan2f call and its scaling, the 60° compare (against kArrowSixty), the site,
/// the 120° compare (kArrowOneTwenty), the side branch and the top/bottom branch's first instruction, as the research
/// read them.
inline constexpr std::uint8_t kBattleArrowBytes[] = {
    0xE8, 0x33, 0x07, 0x83, 0x00, 0xF3, 0x0F, 0x59, 0x05, 0xC3, 0xEB, 0xBA, 0x03, 0xF3, 0x45, 0x0F, 0x59, 0xD5, 0x44,
    0x0F, 0x28, 0x6C, 0x24, 0x70, 0x0F, 0x28, 0xC8, 0x0F, 0x54, 0x0D, 0x46, 0x16, 0xBB, 0x03, 0x0F, 0x2F, 0x0D, 0xBF,
    0xEB, 0xBA, 0x03, 0xF3, 0x45, 0x0F, 0x59, 0xE6, 0xF3, 0x45, 0x0F, 0x58, 0xD7, 0x44, 0x0F, 0x28, 0x7C, 0x24, 0x50,
    0x44, 0x0F, 0x28, 0x74, 0x24, 0x60, 0xF3, 0x44, 0x0F, 0x58, 0x64, 0x24, 0x20, 0x76, 0x39, 0x0F, 0x2F, 0x0D, 0x49,
    0xEC, 0xBA, 0x03, 0x73, 0x30, 0x0F, 0x2F, 0xC6, 0x73, 0x0A, 0xF3, 0x0F, 0x10, 0x0D, 0xFA, 0xEB, 0xBA, 0x03, 0xEB,
    0x08, 0xF3, 0x0F, 0x10, 0x0D, 0x20, 0xF3, 0xBA, 0x03, 0x73, 0x0A, 0x44, 0x0F, 0x28, 0xD6, 0x45, 0x0F, 0x28, 0xC4,
    0xEB, 0x2C, 0xF3, 0x44, 0x0F, 0x10, 0x54, 0x24, 0x30, 0x45, 0x0F, 0x28, 0xC4, 0xEB, 0x1F, 0x44, 0x0F, 0x2F, 0xC6};
static_assert(sizeof(kBattleArrowBytes) == 0x1413E39BD - kBattleArrowCheckStart);

/// The instruction replaced: addss xmm12, [rsp+20h].
inline constexpr std::uint8_t kBattleArrowAdd[] = {0xF3, 0x44, 0x0F, 0x58, 0x64, 0x24, 0x20};

inline constexpr std::size_t kBattleArrowCodeSize = 106;

/// Live addresses: the band flag and the two branches; the game's 60.0 by value. The game path falls into the jump
/// back CodeRedirect appends (to kBattleArrowResume).
inline std::array<std::uint8_t, kBattleArrowCodeSize> BattleArrowCode(const std::uint8_t& band, std::uintptr_t side,
                                                                      std::uintptr_t topBottom, float sixty) noexcept
{
    std::array<std::uint8_t, kBattleArrowCodeSize> code = {
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,        //   0: mov    rcx, &band
        0x80, 0x39, 0x00,                          //  10: cmp    byte [rcx], 0
        0x74, 0x4D,                                //  13: je     game (92)
        0xF3, 0x44, 0x0F, 0x58, 0x64, 0x24, 0x20,  //  15: addss  xmm12, [rsp+20h]   ySide (the replaced instruction)
        0x0F, 0x2F, 0x0D, 0x33, 0, 0, 0,           //  22: comiss xmm1, [rip+vertical below] (80)
        0x72, 0x23,                                //  29: jb     topBottom (66)       straight up or down
        0x0F, 0x2F, 0x0D, 0x2E, 0, 0, 0,           //  31: comiss xmm1, [rip+vertical above] (84)
        0x77, 0x1A,                                //  38: ja     topBottom (66)
        0x44, 0x0F, 0x2F, 0xE6,                    //  40: comiss xmm12, xmm6         ySide < 0: leaves by the top
        0x72, 0x14,                                //  44: jb     topBottom (66)
        0x45, 0x0F, 0x2F, 0xE3,                    //  46: comiss xmm12, xmm11        ySide > L.H - h: by the bottom
        0x77, 0x0E,                                //  50: ja     topBottom (66)
        0xFF, 0x25, 0, 0, 0, 0,                    //  52: side: jmp [rip+0]
        0, 0, 0, 0, 0, 0, 0, 0,                    //  58:   the side branch
        0xFF, 0x25, 0, 0, 0, 0,                    //  66: topBottom: jmp [rip+0]
        0, 0, 0, 0, 0, 0, 0, 0,                    //  72:   the top/bottom branch
        0, 0, 0, 0,                                //  80: vertical below
        0, 0, 0, 0,                                //  84: vertical above
        0, 0, 0, 0,                                //  88: sixty
        0xF3, 0x44, 0x0F, 0x58, 0x64, 0x24, 0x20,  //  92: game: addss xmm12, [rsp+20h]
        0x0F, 0x2F, 0x0D, 0xEE, 0xFF, 0xFF, 0xFF,  //  99: comiss xmm1, [rip+sixty] (88): the game's flags again
    };                                             // 106: (the appended jump back to the game's jbe)
    const auto put = [&code](std::size_t at, const void* value, std::size_t size) { std::memcpy(&code[at], value, size); };
    const auto flag = reinterpret_cast<std::uint64_t>(&band);
    put(2, &flag, sizeof(flag));
    put(58, &side, sizeof(side));
    put(72, &topBottom, sizeof(topBottom));
    put(80, &kVerticalBelow, sizeof(float));
    put(84, &kVerticalAbove, sizeof(float));
    put(88, &sixty, sizeof(float));
    return code;
}

// --- 4. The layout scale -----------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kScaleCheckStart = 0x141509F00;
inline constexpr std::uintptr_t kScaleSite       = 0x141509F08;
inline constexpr std::uintptr_t kScaleResume     = 0x141509F17;

/// The render from the block's start through the instruction after the multiply, as the research read it.
inline constexpr std::uint8_t kScaleBytes[] = {
    0x66, 0x41, 0x0F, 0x6E, 0xC1,                    // movd xmm0, r9d
    0x0F, 0x5B, 0xC0,                                // cvtdq2ps xmm0, xmm0
    0x0F, 0x28, 0xE2,                                // movaps xmm4, xmm2           <- replaced
    0x66, 0x0F, 0x6E, 0xDA,                          // movd xmm3, edx              <- replaced
    0xF3, 0x0F, 0x59, 0x25, 0xED, 0x79, 0xA8, 0x03,  // mulss xmm4, [1/1080]        left in place, unrun
    0x0F, 0x5B, 0xDB};                               // cvtdq2ps xmm3, xmm3         resumed at
static_assert(sizeof(kScaleBytes) == kScaleResume + 3 - kScaleCheckStart);

/// The instructions replaced.
inline constexpr std::uint8_t kScaleReplaced[] = {0x0F, 0x28, 0xE2, 0x66, 0x0F, 0x6E, 0xDA};

/// The replaced instructions, then min(height/1080, width/1920). Its constants sit after a short jump and are read
/// rip-relative, so the bytes are the same wherever they are placed.
inline constexpr std::uint8_t kScaleCode[] = {
    0x0F, 0x28, 0xE2,                                //  0: movaps   xmm4, xmm2
    0x66, 0x0F, 0x6E, 0xDA,                          //  3: movd     xmm3, edx
    0xF3, 0x0F, 0x59, 0x25, 0x12, 0x00, 0x00, 0x00,  //  7: mulss    xmm4, [rip+18]  (next 15, constant at 33)
    0xF3, 0x0F, 0x2A, 0xEA,                          // 15: cvtsi2ss xmm5, edx
    0xF3, 0x0F, 0x59, 0x2D, 0x0A, 0x00, 0x00, 0x00,  // 19: mulss    xmm5, [rip+10]  (next 27, constant at 37)
    0xF3, 0x0F, 0x5D, 0xE5,                          // 27: minss    xmm4, xmm5
    0xEB, 0x08,                                      // 31: jmp short over the constants, to the appended jump back
    0xD6, 0xB9, 0x72, 0x3A,                          // 33: 1/1080, as the game stores it
    0x89, 0x88, 0x08, 0x3A};                         // 37: 1/1920, as the game stores it
static_assert(sizeof(kScaleCode) == 41);

}  // namespace lss::hud::layout_code
