// Host-side tests for render/dxbc_fix.h, against the game's own SSR pixel shaders (tests/data, extracted from two
// D3D11 captures; research/RE-third-view-ghost.md) and small hand-built containers.

#include "render/dxbc_fix.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace lss::render::dxbc_fix;

int g_failures = 0;

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

std::vector<std::uint8_t> Load(const char* name)
{
    std::ifstream file(std::string(LSS_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// The byte offset of the first instruction equal to `tokens`, by a plain search (for checking the swap's place).
std::size_t Find(const std::vector<std::uint8_t>& data, const std::array<std::uint32_t, 8>& tokens)
{
    for (std::size_t at = 0; at + sizeof(tokens) <= data.size(); at += 4)
    {
        if (std::memcmp(data.data() + at, tokens.data(), sizeof(tokens)) == 0)
        {
            return at;
        }
    }
    return 0;
}

void TestGameShaders()
{
    for (const char* name : {"ssr_ps3812.dxbc", "ssr_ps2894.dxbc"})
    {
        std::printf("%s: the stored checksum is reproduced; the doubled corner swapped once; the result checks out\n", name);
        const std::vector<std::uint8_t> original = Load(name);
        EXPECT(original.size() > 1000);
        EXPECT(ChecksumHolds(original));  // the checksum port against the compiler's own (the one-block tail)
        std::vector<std::uint8_t> fixed = original;
        const std::size_t         at    = Find(original, kSsrDoubledCorner);
        EXPECT(at != 0);
        EXPECT(CorrectSsr(fixed) == Swap::Done);
        EXPECT(fixed.size() == original.size());
        EXPECT(ChecksumHolds(fixed));
        EXPECT(std::memcmp(fixed.data() + at, kSsrPixelItself.data(), sizeof(kSsrPixelItself)) == 0);
        std::size_t changed = 0;  // only the instruction and the checksum
        for (std::size_t i = 0; i < fixed.size(); ++i)
        {
            const bool inside = (i >= kChecksumAt && i < kChecksumAt + kChecksumBytes) || (i >= at && i < at + 32);
            if (fixed[i] != original[i])
            {
                ++changed;
                EXPECT(inside);
            }
        }
        EXPECT(changed > 0);
        std::vector<std::uint8_t> again = fixed;  // a corrected shader is not corrected twice
        EXPECT(CorrectSsr(again) == Swap::NotFound && again == fixed);
    }
}

/// A container with one SHEX chunk holding `program` after its version and length tokens.
std::vector<std::uint8_t> Container(const std::vector<std::uint32_t>& program)
{
    std::vector<std::uint32_t> words{0x43425844, 0, 0, 0, 0, 1, 0, 1, 36};  // magic, checksum, version, size, 1 chunk
    words.push_back(0x58454853);                                           // "SHEX"
    words.push_back(static_cast<std::uint32_t>(4 * (program.size() + 2)));
    words.push_back(0x00050050);  // ps_5_0
    words.push_back(static_cast<std::uint32_t>(program.size() + 2));
    words.insert(words.end(), program.begin(), program.end());
    words[6] = static_cast<std::uint32_t>(4 * words.size());
    std::vector<std::uint8_t> bytes(4 * words.size());
    std::memcpy(bytes.data(), words.data(), bytes.size());
    const auto sum = Checksum(bytes.data(), bytes.size());
    std::memcpy(bytes.data() + kChecksumAt, sum.data(), sum.size());
    return bytes;
}

void TestChecksumLongTail()
{
    // (size - 20) % 64 in 56..63 takes the checksum's two-block tail; the game's shaders above both take the short one.
    std::puts("the checksum's two-block tail: a compiler-signed shader (tests/data/checksum_tail56.dxbc) is reproduced");
    const std::vector<std::uint8_t> shader = Load("checksum_tail56.dxbc");
    EXPECT(shader.size() > 20 && (shader.size() - 20) % 64 >= 56);
    EXPECT(ChecksumHolds(shader));
    std::vector<std::uint8_t> bent = shader;  // one byte off: no longer holds
    bent[bent.size() / 2] ^= 1;
    EXPECT(!ChecksumHolds(bent));
}

void TestRefusals()
{
    std::puts("refusals: a stale checksum, twice, missing, inside another instruction, malformed, not a container: "
              "nothing changes");
    const std::vector<std::uint32_t> add(kSsrDoubledCorner.begin(), kSsrDoubledCorner.end());
    const std::vector<std::uint32_t> ret{0x0100003E};
    std::vector<std::uint32_t>       twice = add;
    twice.insert(twice.end(), add.begin(), add.end());
    twice.push_back(ret[0]);
    auto bytes = Container(twice);
    auto kept  = bytes;
    EXPECT(ChecksumHolds(bytes));
    EXPECT(CorrectSsr(bytes) == Swap::MoreThanOnce && bytes == kept);

    std::vector<std::uint32_t> once = add;
    once.push_back(ret[0]);
    bytes = Container(once);
    EXPECT(CorrectSsr(bytes) == Swap::Done && ChecksumHolds(bytes));

    bytes = Container(ret);
    kept  = bytes;
    EXPECT(CorrectSsr(bytes) == Swap::NotFound && bytes == kept);

    // The tokens inside a customdata block are not instructions: never swapped there.
    std::vector<std::uint32_t> data{0x00000035, static_cast<std::uint32_t>(2 + add.size())};
    data.insert(data.end(), add.begin(), add.end());
    data.push_back(ret[0]);
    bytes = Container(data);
    kept  = bytes;
    EXPECT(CorrectSsr(bytes) == Swap::NotFound && bytes == kept);

    // An instruction claiming to run past the program.
    bytes = Container({0x7F000000, ret[0]});
    kept  = bytes;
    EXPECT(CorrectSsr(bytes) == Swap::NoProgram && bytes == kept);

    // The add's tokens as the operands of a longer instruction (a 9-token one): not an instruction start.
    std::vector<std::uint32_t> inside{0x09000000};
    inside.insert(inside.end(), add.begin(), add.end());
    inside.push_back(ret[0]);
    bytes = Container(inside);
    kept  = bytes;
    EXPECT(CorrectSsr(bytes) == Swap::NotFound && bytes == kept);

    // A container whose checksum is stale: never re-signed.
    bytes = Container(once);
    bytes[kChecksumAt] ^= 0xFF;
    kept = bytes;
    EXPECT(CorrectSsr(bytes) == Swap::BadChecksum && bytes == kept);

    std::vector<std::uint8_t> junk(64, 0x11);
    EXPECT(CorrectSsr(junk) == Swap::NotContainer);
    bytes = Container(once);
    bytes.pop_back();  // the stored size no longer matches
    EXPECT(CorrectSsr(bytes) == Swap::NotContainer && !ChecksumHolds(bytes));

    // A chunk table that lies (each re-signed, so only the table is wrong): nothing read past the container.
    const auto resign = [](std::vector<std::uint8_t>& b) {
        const auto sum = Checksum(b.data(), b.size());
        std::memcpy(b.data() + kChecksumAt, sum.data(), sum.size());
    };
    const auto setWord = [](std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t value) {
        std::memcpy(b.data() + at, &value, 4);
    };
    bytes = Container(once);
    setWord(bytes, 28, 0x40000000);  // a chunk count past the container
    resign(bytes);
    EXPECT(CorrectSsr(bytes) == Swap::NotContainer);
    bytes = Container(once);
    setWord(bytes, 32, static_cast<std::uint32_t>(bytes.size()));  // the chunk at the very end
    resign(bytes);
    EXPECT(CorrectSsr(bytes) == Swap::NotContainer);
    bytes = Container(once);
    setWord(bytes, 40, 0x10000);  // the program chunk's size past the container
    resign(bytes);
    EXPECT(CorrectSsr(bytes) == Swap::NoProgram);
    bytes = Container(once);
    setWord(bytes, 48, 1);  // a program shorter than its own two header tokens
    resign(bytes);
    EXPECT(CorrectSsr(bytes) == Swap::NoProgram);
    bytes = Container(once);
    setWord(bytes, 48, 0x1000);  // a program longer than its chunk
    resign(bytes);
    EXPECT(CorrectSsr(bytes) == Swap::NoProgram);
    bytes = Container({0x00000035});  // customdata as the last token: no length to read
    EXPECT(CorrectSsr(bytes) == Swap::NoProgram);
}

}  // namespace

int main()
{
    TestGameShaders();
    TestChecksumLongTail();
    TestRefusals();
    if (g_failures == 0)
    {
        std::printf("dxbc_fix_test: all passed\n");
        return 0;
    }
    std::printf("dxbc_fix_test: %d failure(s)\n", g_failures);
    return 1;
}
