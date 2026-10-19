#pragma once

// The machine code player 2's HUD image needs at two spots that have no call to redirect (RE-hud-two-images.md §3,
// RE-hud-overlay-pass.md §10). Each replaces ONE game instruction inside one aligned 16-byte block through
// platform::CodeRedirect and is host-tested by executing it (tests/image_code_test.cpp).
//
// 1. Per-entry buffers (render thread). FEndMenuRenderer__OnRenderInGameUI_Slot2 draws every entry of the ring slot
//    with pool 2's one vertex/index buffer pair, uploading only on a fresh consume; a view that reuses the slot draws
//    each entry from whatever the pair holds last (the Rebirth hazard: player 1's entry drawn with player 2's vertices).
//    Replaced: `mov rax, [rbp+150h]` (the pool's VB, 7 bytes at 0x143A000E2, resume 0x143A000E9). There r9 = the entry,
//    the IB argument is already at [rsp+28h], and rax is about to hold the VB argument. For player 2's entries (a table
//    of up to four, one per ring slot) the code puts player 2's own pair in both arguments; for every other entry it
//    runs the replaced load. rax and the flags change, nothing else.
//
// 2. The navi map's own ring in player 2's pass (game thread). SEndNaviMapOctagon__OnPaint_Pool0 renders the map into
//    pool 0 (acquire 0x1414FDDA9 .. submit 0x1414FDFBB) inside the overlay paint; a second overlay paint would turn
//    pool 0's ring twice a frame (Rebirth's 32 KB leak). In player 2's pass the span is skipped, as Rebirth's
//    minimap_once: the map widget paints the texture player 1's pass rendered.
//    Replaced: the `call AcquireLayerSlot` (5 bytes at 0x1414FDDA9). There rsi = the painting octagon's paint
//    interface (recorded for the mod's readings). If it is the one named for this pass (the other player's octagon,
//    hud/minimap_two.h; the skip counted): xmm8 = 1.0 (the
//    span sets it; 0x1414FDFD0 compares with it) and a jump past the submit (0x1414FDFC0). Otherwise the call is made
//    exactly as the game makes it, without a call instruction of our own: the game's return address (0x1414FDDAE) is
//    pushed and the code jumps to the acquire (hud/minimap_two's handover), so unwinding inside it sees the game's own
//    frame. After the span the function reloads
//    r12/r13 and every register and stack slot it reads (checked in IDA, RE-hud-overlay-pass.md §10). rax and the
//    flags change (the site is a call: the flags are dead there).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::hud::image_code {

/// Player 2's entries and buffer pair, read by the render-thread code: entries[0..3] (0 = unused), then VB, IB.
struct alignas(16) EntryTable
{
    std::uintptr_t entries[4] = {};
    std::uintptr_t vertexBuffer = 0;  // +0x20
    std::uintptr_t indexBuffer  = 0;  // +0x28
};
static_assert(offsetof(EntryTable, vertexBuffer) == 0x20 && offsetof(EntryTable, indexBuffer) == 0x28);

// --- 1. Per-entry buffers --------------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kBuffersSite   = 0x143A000E2;
inline constexpr std::uintptr_t kBuffersResume = 0x143A000E9;
/// 0x143A000C0 through 0x143A000EE (the loop's argument setup up to the DrawLayerEntry call), as the research read it.
inline constexpr std::uintptr_t kBuffersCheckStart = 0x143A000C0;
inline constexpr std::uint8_t   kBuffersBytes[]    = {
    0x48, 0x8B, 0x85, 0x68, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xCD, 0x4C, 0x8B, 0x0F, 0x4D, 0x8B, 0x47, 0x08, 0x49,
    0x8B, 0x17, 0x44, 0x88, 0x6C, 0x24, 0x30, 0x4F, 0x8B, 0x0C, 0x31, 0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0x8B,
    0x85, 0x50, 0x01, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24, 0x20};
static_assert(sizeof(kBuffersBytes) == 0x143A000EE - kBuffersCheckStart);
/// The instruction replaced: mov rax, [rbp+150h].
inline constexpr std::uint8_t kBuffersLoad[] = {0x48, 0x8B, 0x85, 0x50, 0x01, 0x00, 0x00};

inline constexpr std::size_t kBuffersCodeSize = 65;

inline std::array<std::uint8_t, kBuffersCodeSize> BuffersCode(const EntryTable& table) noexcept
{
    std::array<std::uint8_t, kBuffersCodeSize> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  //  0: mov  rax, &table
        0x4C, 0x3B, 0x08,                    // 10: cmp  r9, [rax]
        0x74, 0x1B,                          // 13: je   p2 (42)
        0x4C, 0x3B, 0x48, 0x08,              // 15: cmp  r9, [rax+8]
        0x74, 0x15,                          // 19: je   p2
        0x4C, 0x3B, 0x48, 0x10,              // 21: cmp  r9, [rax+10h]
        0x74, 0x0F,                          // 25: je   p2
        0x4C, 0x3B, 0x48, 0x18,              // 27: cmp  r9, [rax+18h]
        0x74, 0x09,                          // 31: je   p2
        0x48, 0x8B, 0x85, 0x50, 0x01, 0x00, 0x00,  // 33: mov rax, [rbp+150h] (the replaced instruction)
        0xEB, 0x17,                          // 40: jmp  done (65)
        0x48, 0x8B, 0x40, 0x28,              // 42: p2: mov rax, [rax+28h]   player 2's IB
        0x48, 0x89, 0x44, 0x24, 0x28,        // 46: mov  [rsp+28h], rax      (the IB argument)
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // 51: mov  rax, &table
        0x48, 0x8B, 0x40, 0x20,              // 61: mov  rax, [rax+20h]      player 2's VB
    };                                       // 65: done (the jump back to 0x143A000E9 is appended here)
    const auto address = reinterpret_cast<std::uint64_t>(&table);
    std::memcpy(&code[2], &address, sizeof(address));
    std::memcpy(&code[53], &address, sizeof(address));
    return code;
}

// --- 2. The navi map's pool 0 -----------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kNaviSite     = 0x1414FDDA9;
inline constexpr std::uintptr_t kNaviReturn   = 0x1414FDDAE;  // the call's own return address
inline constexpr std::uintptr_t kNaviSkipTo   = 0x1414FDFC0;  // just after the pool 0 submit
inline constexpr std::uintptr_t kAcquireLayer = 0x1439FBA10;  // FEndMenuRenderer__AcquireLayerSlot
/// 0x1414FDDA4 through 0x1414FDFC0 is checked by the installer in two parts: the call and its setup here, and the
/// submit at the end of the span (kNaviSubmitBytes at 0x1414FDFB3).
inline constexpr std::uintptr_t kNaviCheckStart = 0x1414FDDA4;
inline constexpr std::uint8_t   kNaviBytes[]    = {0x33, 0xD2, 0x49, 0x8B, 0xCC, 0xE8, 0x62, 0xDC, 0x4F, 0x02};
inline constexpr std::uintptr_t kNaviSubmitStart = 0x1414FDFB3;
inline constexpr std::uint8_t   kNaviSubmitBytes[] = {0x4D, 0x8B, 0xC5, 0x33, 0xD2, 0x49, 0x8B, 0xCC, 0xE8, 0x90, 0xE7, 0x4F, 0x02};
/// The instruction replaced: call FEndMenuRenderer__AcquireLayerSlot.
inline constexpr std::uint8_t kNaviCall[] = {0xE8, 0x62, 0xDC, 0x4F, 0x02};

/// What the navi code reads and records (game thread): the paint interface whose span is skipped in this pass (0:
/// none), then, for the mod's readings, the last paint interface that reached the site and the skips made.
struct alignas(16) NaviState
{
    std::uintptr_t skip  = 0;
    std::uintptr_t seen  = 0;  // +0x08
    std::uint64_t  skips = 0;  // +0x10
};
static_assert(offsetof(NaviState, seen) == 0x08 && offsetof(NaviState, skips) == 0x10);

inline constexpr std::size_t kNaviCodeSize = 83;

/// Live addresses: the state above, the call's return address, the acquire function (the game's, or a replacement
/// with its signature) and the point after the submit.
inline std::array<std::uint8_t, kNaviCodeSize> NaviCode(NaviState& state, std::uintptr_t returnAddress,
                                                        std::uintptr_t acquire, std::uintptr_t skipTo) noexcept
{
    std::array<std::uint8_t, kNaviCodeSize> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        //  0: mov   rax, &state
        0x48, 0x89, 0x70, 0x08,                    // 10: mov   [rax+8], rsi        the painting octagon, recorded
        0x48, 0x3B, 0x30,                          // 14: cmp   rsi, [rax]          the one named for this pass?
        0x74, 0x21,                                // 17: je    skip (52)
        0x48, 0x83, 0xEC, 0x08,                    // 19: sub   rsp, 8                  the call, made as the game makes it:
        0xC7, 0x04, 0x24, 0, 0, 0, 0,              // 23: mov   dword [rsp], low        its return address
        0xC7, 0x44, 0x24, 0x04, 0, 0, 0, 0,        // 30: mov   dword [rsp+4], high
        0xFF, 0x25, 0, 0, 0, 0,                    // 38: jmp   [rip+0]                 AcquireLayerSlot
        0, 0, 0, 0, 0, 0, 0, 0,                    // 44:   the function
        0x48, 0xFF, 0x40, 0x10,                    // 52: skip: inc qword [rax+10h]      counted
        0xF3, 0x44, 0x0F, 0x10, 0x05, 14, 0, 0, 0, // 56: movss xmm8, [rip+one] (79)
        0xFF, 0x25, 0, 0, 0, 0,                    // 65: jmp   [rip+0]                 past the submit
        0, 0, 0, 0, 0, 0, 0, 0,                    // 71:   the point
        0x00, 0x00, 0x80, 0x3F,                    // 79: one: 1.0f
    };                                             // 83: (the appended jump back is never reached)
    const auto put = [&code](std::size_t at, const void* value, std::size_t size) { std::memcpy(&code[at], value, size); };
    const auto where = reinterpret_cast<std::uint64_t>(&state);
    const auto low   = static_cast<std::uint32_t>(returnAddress);
    const auto high  = static_cast<std::uint32_t>(returnAddress >> 32);
    put(2, &where, sizeof(where));
    put(26, &low, sizeof(low));
    put(34, &high, sizeof(high));
    put(44, &acquire, sizeof(acquire));
    put(71, &skipTo, sizeof(skipTo));
    return code;
}

}  // namespace lss::hud::image_code
