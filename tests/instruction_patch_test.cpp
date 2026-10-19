// Host-side tests for platform/instruction_patch.h, run without the game: the distortion apply's target-size load is
// placed at the game's block offset inside a stand-in function, patched, EXECUTED, and restored.

#include "engine_facts.h"
#include "platform/instruction_patch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

namespace facts = lss::facts;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

// std::uint64_t Load(std::uint64_t buffer, std::uint64_t view): stores buffer at [rsp+50h] and view at [rsp+70h], as
// the game stores TextureSize and the view size, then runs the site and returns rax (the TargetSize argument).
struct StandIn
{
    using Fn = std::uint64_t (*)(std::uint64_t buffer, std::uint64_t view);
    Fn            fn   = nullptr;
    std::uint8_t* site = nullptr;

    StandIn()
    {
        std::vector<std::uint8_t> b;
        auto put = [&b](std::initializer_list<std::uint8_t> bytes) { b.insert(b.end(), bytes); };
        put({0x48, 0x83, 0xEC, 0x78});        // sub rsp, 78h
        put({0x48, 0x89, 0x4C, 0x24, 0x50});  // mov [rsp+50h], rcx
        put({0x48, 0x89, 0x54, 0x24, 0x70});  // mov [rsp+70h], rdx
        while ((b.size() & 0xF) != (facts::kDistortionApplyTargetLoad & 0xF))
        {
            b.push_back(0x90);
        }
        const std::size_t at = b.size();
        b.insert(b.end(), std::begin(facts::kDistortionApplyTargetView), std::end(facts::kDistortionApplyTargetView));
        put({0x48, 0x83, 0xC4, 0x78, 0xC3});  // add rsp, 78h; ret
        auto* page = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        std::memcpy(page, b.data(), b.size());
        fn   = reinterpret_cast<Fn>(page);
        site = page + at;
    }
};

void TestPatchRunsAndRestores()
{
    std::printf("patch, run, restore\n");
    StandIn                        s;
    lss::platform::InstructionPatch patch;
    const auto                     site = reinterpret_cast<std::uintptr_t>(s.site);
    EXPECT(s.fn(1920 | (1080ull << 32), 1920 | (540ull << 32)) == (1920 | (540ull << 32)));  // the game: view size
    EXPECT(patch.Apply(site, facts::kDistortionApplyTargetView, facts::kDistortionApplyTargetBuffer));
    EXPECT(patch.IsApplied());
    EXPECT(s.fn(1920 | (1080ull << 32), 1920 | (540ull << 32)) == (1920 | (1080ull << 32)));  // patched: buffer size
    EXPECT(!patch.Apply(site, facts::kDistortionApplyTargetView, facts::kDistortionApplyTargetBuffer));  // once only
    EXPECT(patch.Restore());
    EXPECT(!patch.IsApplied());
    EXPECT(s.fn(7, 9) == 9);
    EXPECT(patch.Apply(site, facts::kDistortionApplyTargetView, facts::kDistortionApplyTargetBuffer));  // again
    EXPECT(s.fn(7, 9) == 7);
}

void TestRefusals()
{
    std::printf("refusals\n");
    StandIn                        s;
    lss::platform::InstructionPatch patch;
    const auto                     site = reinterpret_cast<std::uintptr_t>(s.site);
    // Bytes that are not the research's: nothing changes.
    EXPECT(!patch.Apply(site, facts::kDistortionApplyTargetBuffer, facts::kDistortionApplyTargetView));
    // A different length.
    const std::uint8_t shorter[] = {0x48, 0x8B, 0x44, 0x24};
    EXPECT(!patch.Apply(site, facts::kDistortionApplyTargetView, shorter));
    // Crossing the block: the same five bytes one byte later end past the block.
    EXPECT(!patch.Apply(site + 1, facts::kDistortionApplyTargetView, facts::kDistortionApplyTargetBuffer));
    EXPECT(!patch.IsApplied());
    EXPECT(s.fn(7, 9) == 9);
    // Restore when the bytes changed under the patch: left alone.
    EXPECT(patch.Apply(site, facts::kDistortionApplyTargetView, facts::kDistortionApplyTargetBuffer));
    s.site[4] = 0x60;  // someone else's change
    EXPECT(!patch.Restore());
    EXPECT(s.site[4] == 0x60);
}

}  // namespace

int main()
{
    TestPatchRunsAndRestores();
    TestRefusals();
    std::printf(g_failures == 0 ? "all passed\n" : "%d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
