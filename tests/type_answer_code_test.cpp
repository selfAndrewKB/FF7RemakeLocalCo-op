// Host-side tests for the leader-type answer's code (battle/type_answer_code.h): the bytes for each register, every
// conditional jump landing on the hit exit, the addresses in place, and the refusals. Run without the game.

#include "battle/type_answer_code.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "type_answer_code: all passed\n" : "type_answer_code: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
}  // namespace lss::test

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++lss::test::g_failures;                                         \
        }                                                                    \
    } while (false)

namespace {

using namespace lss::battle::type_answer_code;

constexpr std::uintptr_t kBlocks = 0x7FF6A0001000;
constexpr std::uintptr_t kPmGet  = 0x7FF6B0657AC0;

std::uint64_t Qword(const std::uint8_t* at)
{
    std::uint64_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

/// Walks the built code: checks each instruction's opcode and that every `je` lands on `mov rax, rcx`.
void Walk(Register reg, std::size_t blocks)
{
    std::array<std::uint8_t, kMaxBytes> code{};
    const std::size_t                   length = Build(code, reg, kBlocks, blocks, kPmGet);
    EXPECT(length != 0);
    std::size_t at = 0;
    if (reg.rex != 0)
    {
        EXPECT(code[at++] == reg.rex);
    }
    EXPECT(code[at] == 0x0F && code[at + 1] == 0xB6 && code[at + 2] == reg.modrm && code[at + 3] == 0x58);
    at += 4;
    EXPECT(code[at] == 0x48 && code[at + 1] == 0xB9);
    EXPECT(Qword(&code[at + 2]) == kBlocks);
    at += 10;
    const std::size_t hit = length - 4;
    for (std::size_t i = 0; i < blocks; ++i)
    {
        EXPECT(code[at] == 0x3A && code[at + 1] == 0x81 && code[at + 2] == 0x33 && code[at + 3] == 0x02);
        at += 6;
        EXPECT(code[at] == 0x74);
        EXPECT(at + 2 + code[at + 1] == hit);
        at += 2;
        if (i + 1 < blocks)
        {
            EXPECT(code[at] == 0x48 && code[at + 1] == 0x81 && code[at + 2] == 0xC1 && code[at + 3] == 0x58 && code[at + 4] == 0x02);
            at += 7;
        }
    }
    EXPECT(code[at] == 0xFF && code[at + 1] == 0x25);
    EXPECT(code[at + 2] == 0 && code[at + 3] == 0 && code[at + 4] == 0 && code[at + 5] == 0);  // rel32 0: the address follows
    EXPECT(Qword(&code[at + 6]) == kPmGet);
    at += 14;
    EXPECT(at == hit);
    EXPECT(code[hit] == 0x48 && code[hit + 1] == 0x8B && code[hit + 2] == 0xC1 && code[hit + 3] == 0xC3);
}

void Refusals()
{
    std::array<std::uint8_t, kMaxBytes> code{};
    EXPECT(Build(code, kR14, kBlocks, 0, kPmGet) == 0);
    EXPECT(Build(code, kR14, kBlocks, kMaxBlocks + 1, kPmGet) == 0);
    std::array<std::uint8_t, 20> small{};
    EXPECT(Build(small, kR14, kBlocks, 1, kPmGet) == 0);
}

void Lengths()
{
    std::array<std::uint8_t, kMaxBytes> code{};
    EXPECT(Build(code, kRdi, kBlocks, 1, kPmGet) == 4 + 10 + 8 + 14 + 4);
    EXPECT(Build(code, kR14, kBlocks, 3, kPmGet) == 5 + 10 + 3 * 8 + 2 * 7 + 14 + 4);
}

}  // namespace

int main()
{
    Walk(kR14, 3);
    Walk(kRdi, 3);
    Walk(kRbx, 1);
    Walk(kRdi, 2);
    Refusals();
    Lengths();
    return lss::test::Report();
}
