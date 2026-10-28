#pragma once

// The machine code the further players' HUD images need at two spots that have no call to redirect (RE-hud-two-images.md §3,
// RE-hud-overlay-pass.md §10). Each replaces ONE game instruction inside one aligned 16-byte block through
// platform::CodeRedirect and is host-tested by executing it (tests/image_code_test.cpp).
//
// 1. Per-entry buffers (render thread). FEndMenuRenderer__OnRenderInGameUI_Slot2 draws every entry of the ring slot
//    with pool 2's one vertex/index buffer pair, uploading only on a fresh consume; a view that reuses the slot draws
//    each entry from whatever the pair holds last (the Rebirth hazard: player 1's entry drawn with player 2's vertices).
//    Replaced: `mov rax, [rbp+150h]` (the pool's VB, 7 bytes at 0x143A000E2, resume 0x143A000E9). There r9 = the entry,
//    the IB argument is already at [rsp+28h], and rax is about to hold the VB argument. Each further column has a table
//    (up to four entries, one per ring slot, and its own pair): for an entry in a column's table the code puts that
//    column's pair in both arguments; for every other entry it runs the replaced load. rax and the flags change,
//    nothing else.
//
// 2. The navi map's own ring in a further player's pass (game thread). SEndNaviMapOctagon__OnPaint_Pool0 renders the map into
//    pool 0 (acquire 0x1414FDDA9 .. submit 0x1414FDFBB) inside the overlay paint; a second overlay paint would turn
//    pool 0's ring twice a frame (Rebirth's 32 KB leak). In a further player's pass the span is skipped, as Rebirth's
//    minimap_once: the map widget paints the texture player 1's pass rendered.
//    Replaced: the `call AcquireLayerSlot` (5 bytes at 0x1414FDDA9). There rsi = the painting octagon's paint
//    interface (recorded for the mod's readings). If it is one of those named for this pass (every other player's
//    octagon, up to three, hud/minimap.h; the skip counted): xmm8 = 1.0 (the
//    span sets it; 0x1414FDFD0 compares with it) and a jump past the submit (0x1414FDFC0). Otherwise the call is made
//    exactly as the game makes it, without a call instruction of our own: the game's return address (0x1414FDDAE) is
//    pushed and the code jumps to the acquire (hud/minimap's handover), so unwinding inside it sees the game's own
//    frame. After the span the function reloads
//    r12/r13 and every register and stack slot it reads (checked in IDA, RE-hud-overlay-pass.md §10). rax and the
//    flags change (the site is a call: the flags are dead there).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace lss::hud::image_code {

/// One further column's entries and buffer pair, read by the render-thread code: entries[0..3] (0 = unused), then
/// VB, IB.
struct alignas(16) EntryTable
{
    std::uintptr_t entries[4] = {};
    std::uintptr_t vertexBuffer = 0;  // +0x20
    std::uintptr_t indexBuffer  = 0;  // +0x28
};
static_assert(offsetof(EntryTable, vertexBuffer) == 0x20 && offsetof(EntryTable, indexBuffer) == 0x28);

/// The further columns' tables, column 1's first (the code reads them by address, so they live as long as the code).
inline constexpr std::size_t kTables = 2;
using EntryTables = std::array<EntryTable, kTables>;

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

/// Per table: a check block (33 bytes: `mov rax, &table`, then `cmp r9, [rax+8k]; je hit` for its four entries); after
/// the last, the replaced load and a jump to the end (9); then per table a hit block (`mov rax, [rax+28h]` its IB,
/// `mov [rsp+28h], rax`, `mov rax, &table`, `mov rax, [rax+20h]` its VB: 23 bytes, and a 2-byte jump to the end for
/// every table but the last). Every jump is a short one.
inline constexpr std::size_t kBuffersCheckBytes = 33;
inline constexpr std::size_t kBuffersHitBytes   = 23;
inline constexpr std::size_t kBuffersCodeSize   = kTables * kBuffersCheckBytes + 9 + kTables * kBuffersHitBytes + (kTables - 1) * 2;
static_assert(kBuffersCodeSize < 128, "every jump in the buffers code is a short one");

inline std::array<std::uint8_t, kBuffersCodeSize> BuffersCode(const EntryTables& tables) noexcept
{
    std::array<std::uint8_t, kBuffersCodeSize> code{};
    std::size_t at = 0;
    const auto put = [&code, &at](std::initializer_list<std::uint8_t> bytes) {
        for (const std::uint8_t b : bytes)
        {
            code[at++] = b;
        }
    };
    const auto putAddress = [&code, &at](const EntryTable& table) {
        const auto address = reinterpret_cast<std::uint64_t>(&table);
        std::memcpy(&code[at], &address, sizeof(address));
        at += sizeof(address);
    };
    const std::size_t hits = kTables * kBuffersCheckBytes + 9;  // the first hit block
    const auto hitOf = [hits](std::size_t t) { return hits + t * (kBuffersHitBytes + 2); };
    // The check blocks: a je's rel8 is the hit block's start minus the end of the je.
    for (std::size_t t = 0; t < kTables; ++t)
    {
        put({0x48, 0xB8});  // mov rax, &tables[t]
        putAddress(tables[t]);
        for (std::uint8_t k = 0; k < 4; ++k)
        {
            if (k == 0)
            {
                put({0x4C, 0x3B, 0x08});  // cmp r9, [rax]
            }
            else
            {
                put({0x4C, 0x3B, 0x48, static_cast<std::uint8_t>(8 * k)});  // cmp r9, [rax+8k]
            }
            put({0x74, static_cast<std::uint8_t>(hitOf(t) - (at + 2))});  // je hit t
        }
    }
    put({0x48, 0x8B, 0x85, 0x50, 0x01, 0x00, 0x00});                  // mov rax, [rbp+150h] (the replaced instruction)
    put({0xEB, static_cast<std::uint8_t>(kBuffersCodeSize - (at + 2))});  // jmp done
    for (std::size_t t = 0; t < kTables; ++t)
    {
        put({0x48, 0x8B, 0x40, 0x28});        // hit t: mov rax, [rax+28h]   the column's IB
        put({0x48, 0x89, 0x44, 0x24, 0x28});  // mov [rsp+28h], rax          (the IB argument)
        put({0x48, 0xB8});                    // mov rax, &tables[t]
        putAddress(tables[t]);
        put({0x48, 0x8B, 0x40, 0x20});        // mov rax, [rax+20h]          the column's VB
        if (t + 1 < kTables)
        {
            put({0xEB, static_cast<std::uint8_t>(kBuffersCodeSize - (at + 2))});  // jmp done
        }
    }
    return code;  // at == kBuffersCodeSize: done (the jump back to 0x143A000E9 is appended here)
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

/// What the navi code reads and records (game thread): the paint interfaces whose spans are skipped in this pass (0:
/// none; `skip` and `skipMore`, every other player's octagon), then, for the mod's readings, the last paint interface
/// that reached the site and the skips made.
struct alignas(16) NaviState
{
    std::uintptr_t skip        = 0;
    std::uintptr_t seen        = 0;   // +0x08
    std::uint64_t  skips       = 0;   // +0x10
    std::uintptr_t skipMore[2] = {};  // +0x18, +0x20
};
static_assert(offsetof(NaviState, seen) == 0x08 && offsetof(NaviState, skips) == 0x10 &&
              offsetof(NaviState, skipMore) == 0x18);
inline constexpr std::size_t kNaviSkips = 3;  // `skip` and `skipMore`

inline constexpr std::size_t kNaviCodeSize = 95;

/// Live addresses: the state above, the call's return address, the acquire function (the game's, or a replacement
/// with its signature) and the point after the submit.
inline std::array<std::uint8_t, kNaviCodeSize> NaviCode(NaviState& state, std::uintptr_t returnAddress,
                                                        std::uintptr_t acquire, std::uintptr_t skipTo) noexcept
{
    std::array<std::uint8_t, kNaviCodeSize> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        //  0: mov   rax, &state
        0x48, 0x89, 0x70, 0x08,                    // 10: mov   [rax+8], rsi        the painting octagon, recorded
        0x48, 0x3B, 0x30,                          // 14: cmp   rsi, [rax]          one named for this pass?
        0x74, 0x2D,                                // 17: je    skip (64)
        0x48, 0x3B, 0x70, 0x18,                    // 19: cmp   rsi, [rax+18h]
        0x74, 0x27,                                // 23: je    skip
        0x48, 0x3B, 0x70, 0x20,                    // 25: cmp   rsi, [rax+20h]
        0x74, 0x21,                                // 29: je    skip
        0x48, 0x83, 0xEC, 0x08,                    // 31: sub   rsp, 8                  the call, made as the game makes it:
        0xC7, 0x04, 0x24, 0, 0, 0, 0,              // 35: mov   dword [rsp], low        its return address
        0xC7, 0x44, 0x24, 0x04, 0, 0, 0, 0,        // 42: mov   dword [rsp+4], high
        0xFF, 0x25, 0, 0, 0, 0,                    // 50: jmp   [rip+0]                 AcquireLayerSlot
        0, 0, 0, 0, 0, 0, 0, 0,                    // 56:   the function
        0x48, 0xFF, 0x40, 0x10,                    // 64: skip: inc qword [rax+10h]      counted
        0xF3, 0x44, 0x0F, 0x10, 0x05, 14, 0, 0, 0, // 68: movss xmm8, [rip+one] (91)
        0xFF, 0x25, 0, 0, 0, 0,                    // 77: jmp   [rip+0]                 past the submit
        0, 0, 0, 0, 0, 0, 0, 0,                    // 83:   the point
        0x00, 0x00, 0x80, 0x3F,                    // 91: one: 1.0f
    };                                             // 95: (the appended jump back is never reached)
    const auto put = [&code](std::size_t at, const void* value, std::size_t size) { std::memcpy(&code[at], value, size); };
    const auto where = reinterpret_cast<std::uint64_t>(&state);
    const auto low   = static_cast<std::uint32_t>(returnAddress);
    const auto high  = static_cast<std::uint32_t>(returnAddress >> 32);
    put(2, &where, sizeof(where));
    put(38, &low, sizeof(low));
    put(46, &high, sizeof(high));
    put(56, &acquire, sizeof(acquire));
    put(83, &skipTo, sizeof(skipTo));
    return code;
}

}  // namespace lss::hud::image_code
