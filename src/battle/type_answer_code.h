#pragma once

// The bytes of a small function that answers the game's inlined "is this member the player" test for every human's
// member (battle/enemy_coin.h; RE-enemy-attention-coin.md §4.3, rule B). Pure: the bytes are built here and tested on
// the host (tests/type_answer_code_test.cpp); enemy_coin places them near the game's code and points the call at them.
//
// The test the game compiled inline, at four places (the hate rows, the two tie-break reads, IsAIPC), is
//     call PartyManager_Get ; movzx ecx, byte [rax+0x233] ; cmp [member+0x58], cl
// the party manager's leader type against the member's own type, with the manager pointer used for nothing else.
// The function called in place of PartyManager_Get reads the member's type from the register the game holds it in
// and, when it is a further human's member, returns a block of the mod's own whose +0x233 holds that same type (so
// the compare says "the player"); otherwise it jumps to the game's PartyManager_Get. The blocks (one per further
// seat, 0x258 bytes each, contiguous) are the mod's: the type byte is written on the game thread when a seat
// changes, 9 (no member) never matching a real type. The code touches only rax and rcx, both scratch at a call, and
// never the stack, so the game's function runs on exactly as before.
//
//     movzx eax, byte [member+0x58]
//     mov rcx, <first block>
//   per block:
//     cmp al, [rcx+0x233] ; je hit ; (add rcx, 0x258 before the next)
//     jmp [rip+0] ; dq PartyManager_Get
//   hit:
//     mov rax, rcx ; ret

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>

namespace lss::battle::type_answer_code {

inline constexpr std::uint32_t kTypeOffset = 0x233;  // the leader type's offset in the party manager
inline constexpr std::size_t   kBlockBytes = 0x258;  // the party manager's size: a block answers as one
inline constexpr std::uint32_t kMemberType = 0x58;   // the member's type on its battle component
inline constexpr std::size_t   kMaxBlocks  = 3;      // seats 1..3
inline constexpr std::size_t   kMaxBytes   = 96;

/// The register the game holds the member's battle component in at the call: its REX prefix (0 for none) and the
/// ModRM byte of `movzx eax, byte [reg+disp8]`.
struct Register
{
    std::uint8_t rex;
    std::uint8_t modrm;
};
inline constexpr Register kR14{0x41, 0x46};
inline constexpr Register kRdi{0x00, 0x47};
inline constexpr Register kRbx{0x00, 0x43};

/// Writes the function into `out`; returns its length, or 0 if `blocks` is 0 or over kMaxBlocks or `out` is short.
inline std::size_t Build(std::span<std::uint8_t> out, Register member, std::uintptr_t firstBlock, std::size_t blocks,
                         std::uintptr_t partyManagerGet) noexcept
{
    if (blocks == 0 || blocks > kMaxBlocks)
    {
        return 0;
    }
    const std::size_t head   = (member.rex != 0 ? 1 : 0) + 4 + 10;  // movzx, mov rcx
    const std::size_t tests  = blocks * 8 + (blocks - 1) * 7;       // cmp + je each, add between
    const std::size_t hit    = head + tests + 14;                    // after the jump and its address
    const std::size_t length = hit + 4;
    if (length > out.size())
    {
        return 0;
    }
    std::size_t at = 0;
    auto        put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (const std::uint8_t b : bytes)
        {
            out[at++] = b;
        }
    };
    if (member.rex != 0)
    {
        put({member.rex});
    }
    put({0x0F, 0xB6, member.modrm, static_cast<std::uint8_t>(kMemberType)});
    put({0x48, 0xB9});
    std::memcpy(out.data() + at, &firstBlock, sizeof(firstBlock));
    at += sizeof(firstBlock);
    for (std::size_t i = 0; i < blocks; ++i)
    {
        put({0x3A, 0x81, 0x33, 0x02, 0x00, 0x00});  // cmp al, [rcx+0x233]
        put({0x74, static_cast<std::uint8_t>(hit - (at + 2))});
        if (i + 1 < blocks)
        {
            put({0x48, 0x81, 0xC1, 0x58, 0x02, 0x00, 0x00});  // add rcx, 0x258
        }
    }
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});  // jmp [rip+0]
    std::memcpy(out.data() + at, &partyManagerGet, sizeof(partyManagerGet));
    at += sizeof(partyManagerGet);
    put({0x48, 0x8B, 0xC1, 0xC3});  // mov rax, rcx ; ret
    return at == length ? length : 0;
}

}  // namespace lss::battle::type_answer_code
