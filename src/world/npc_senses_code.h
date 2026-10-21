#pragma once

// The pieces of world/npc_senses that need no game: the stimulus grid's cell rule, the choice of its centre, and the
// machine code that puts that centre in place (research/RE-npc-reactions.md §2, §9). Host-tested
// (tests/npc_senses_code_test.cpp; the code is executed).
//
// The NPCs' senses look through one grid of 15 x 15 x 3 cells of 4 m that the game centres every field tick on
// player 1's character (EndInterMgr_BuildGrid_CentreOnPC0Pawn: PlayerControllerList[0]'s pawn). A character outside
// it is seen by no NPC. The game has no second grid and no other centre, so when player 2 would fall outside, the
// mod moves the centre toward player 2, only as far as keeps player 1 at least a cell inside the edge (player 1
// first), never in height and never in battle (the grid also holds the battle heat map). That choice is the one
// recreated piece (documented in the plan's world topic); the grid build, the senses and the reactions are the game's.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <xmmintrin.h>

namespace lss::world::npc_senses_code {

// --- The cell rule and the centre ------------------------------------------------------------------------------------

inline constexpr int kHalfCells = 7;  // the grid holds cells -7..7 around its centre (x, y); -1..1 in height
inline constexpr int kReach     = 6;  // how far from the centre the mod lets either player sit: one cell inside

struct Cell
{
    int x = 0, y = 0, z = 0;
    friend bool operator==(const Cell&, const Cell&) = default;
};

/// A coordinate's cell exactly as the game rounds it: cvtss2si((v * 0.0025 + v * 0.0025) + 0.5) >> 1.
inline int CellOf(float v) noexcept
{
    const float twice = (v * 0.0025f + v * 0.0025f) + 0.5f;
    return _mm_cvtss_si32(_mm_set_ss(twice)) >> 1;
}

inline Cell CellOf(const float (&location)[3]) noexcept
{
    return {CellOf(location[0]), CellOf(location[1]), CellOf(location[2])};
}

enum class Fit : std::uint8_t
{
    Inside,       // player 2 is within reach of player 1's centre: the game's centre stands
    Shifted,      // the centre moved toward player 2, player 1 still within reach
    TooFar,       // more than two reaches apart across the ground: the game's centre (player 1 first)
    OtherFloor,   // more than one cell apart in height: the game's centre
};

struct Choice
{
    Fit  fit = Fit::Inside;
    Cell centre;  // the centre to use (player 1's cell unless Shifted)
};

/// The centre for player 1's cell `one` and player 2's cell `two`.
inline Choice ChooseCentre(Cell one, Cell two) noexcept
{
    if (std::abs(two.z - one.z) > 1)
    {
        return {Fit::OtherFloor, one};
    }
    bool       tooFar = false;
    const auto axis   = [&tooFar](int a, int b) {
        const int d = b - a;
        if (d > 2 * kReach || d < -2 * kReach)
        {
            tooFar = true;
        }
        return d > kReach ? b - kReach : d < -kReach ? b + kReach : a;
    };
    const Cell centre{axis(one.x, two.x), axis(one.y, two.y), one.z};
    if (tooFar)
    {
        return {Fit::TooFar, one};
    }
    return {centre == one ? Fit::Inside : Fit::Shifted, centre};
}

// --- The code at the grid build -------------------------------------------------------------------------------------
// Replaced: `movss xmm2, [rip+...]` (the 400.0f cell size, 8 bytes at 0x1406B6401, resume 0x1406B6409), the join
// right after the game stores its centre (rbx = the manager; mgr+0x16718/0x1671C/0x16720 are read from memory by
// everything after). If the state's `on` byte is set, the code writes the state's three cells over the game's and
// counts the write; then it loads the same 400.0f from the game's own constant. rax, edx and the flags change: the
// next instructions overwrite all three (0x1406B6410 `lea eax`, 0x1406B641A `imul edx`, `imul`/`sub` the flags).

inline constexpr std::uintptr_t kCentreSite     = 0x1406B6401;
inline constexpr std::uintptr_t kCentreResume   = 0x1406B6409;
inline constexpr std::uintptr_t kCellSizeGlobal = 0x144F92728;  // the 400.0f the replaced instruction loads
/// 0x1406B63E9 through 0x1406B6410 (the centre's stores, the replaced load, the loop setup), as the research read it.
inline constexpr std::uintptr_t kCentreCheckStart = 0x1406B63E9;
inline constexpr std::uint8_t   kCentreBytes[]    = {
    0xF2, 0x0F, 0x11, 0x83, 0x18, 0x67, 0x01, 0x00,  // movsd [rbx+16718h], xmm0   the x and y cells
    0xF3, 0x0F, 0x58, 0xCB,                          // addss xmm1, xmm3
    0xF3, 0x0F, 0x2D, 0xC1,                          // cvtss2si eax, xmm1
    0xD1, 0xF8,                                      // sar eax, 1
    0x89, 0x83, 0x20, 0x67, 0x01, 0x00,              // mov [rbx+16720h], eax       the height cell
    0xF3, 0x0F, 0x10, 0x15, 0x1F, 0xC3, 0x8D, 0x04,  // movss xmm2, [400.0f]         (the site)
    0x41, 0xBE, 0xFF, 0xFF, 0xFF, 0xFF,              // mov r14d, -1
    0x90};                                           // nop; 0x1406B6410 is the loop head
static_assert(sizeof(kCentreBytes) == 0x1406B6410 - kCentreCheckStart);
/// The instruction replaced.
inline constexpr std::uint8_t kCentreLoad[] = {0xF3, 0x0F, 0x10, 0x15, 0x1F, 0xC3, 0x8D, 0x04};

inline constexpr std::uint32_t kManagerCentre = 0x16718;  // the three i32 cells

/// What the code reads (written by the mod on the game thread, read by the code on the game thread).
struct alignas(16) CentreState
{
    std::int32_t  x = 0, y = 0, z = 0;
    std::uint8_t  on = 0;      // +0x0C
    std::uint64_t writes = 0;  // +0x10: counted by the code
};
static_assert(offsetof(CentreState, on) == 0x0C && offsetof(CentreState, writes) == 0x10);

inline constexpr std::size_t kCentreCodeSize = 60;

/// `cellSize` is the live address of the game's 400.0f (kCellSizeGlobal rebased).
inline std::array<std::uint8_t, kCentreCodeSize> CentreCode(CentreState& state, std::uintptr_t cellSize) noexcept
{
    std::array<std::uint8_t, kCentreCodeSize> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  //  0: mov   rax, &state
        0x80, 0x78, 0x0C, 0x00,              // 10: cmp   byte [rax+0Ch], 0
        0x74, 0x1E,                          // 14: je    load (46)
        0x8B, 0x10,                          // 16: mov   edx, [rax]
        0x89, 0x93, 0x18, 0x67, 0x01, 0x00,  // 18: mov   [rbx+16718h], edx
        0x8B, 0x50, 0x04,                    // 24: mov   edx, [rax+4]
        0x89, 0x93, 0x1C, 0x67, 0x01, 0x00,  // 27: mov   [rbx+1671Ch], edx
        0x8B, 0x50, 0x08,                    // 33: mov   edx, [rax+8]
        0x89, 0x93, 0x20, 0x67, 0x01, 0x00,  // 36: mov   [rbx+16720h], edx
        0x48, 0xFF, 0x40, 0x10,              // 42: inc   qword [rax+10h]
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // 46: load: mov rax, &400.0f (the game's constant)
        0xF3, 0x0F, 0x10, 0x10,              // 56: movss xmm2, [rax]    (the replaced load)
    };                                       // 60: the jump back to 0x1406B6409 is appended here
    const auto stateAddress = reinterpret_cast<std::uint64_t>(&state);
    const auto sizeAddress  = static_cast<std::uint64_t>(cellSize);
    std::memcpy(&code[2], &stateAddress, sizeof(stateAddress));
    std::memcpy(&code[48], &sizeAddress, sizeof(sizeAddress));
    return code;
}

}  // namespace lss::world::npc_senses_code
