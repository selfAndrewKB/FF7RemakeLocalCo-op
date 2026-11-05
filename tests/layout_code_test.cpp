// Host-side tests for hud/layout_code.h, run without the game.
//
// The bytes the mod places near the game are EXECUTED here: each is installed through platform::CodeRedirect over a
// stand-in function that loads a case into the registers the game has at the site, holds the replaced instruction at
// the game's offset within its 16-byte block, and reports what the code did (the box written, or the branch taken
// and the registers the game's following instructions set).

#include "hud/layout_code.h"
#include "platform/code_redirect.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

namespace code = lss::hud::layout_code;

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

/// A few bytes of machine code at a time, with padding to a block offset.
struct Builder
{
    std::vector<std::uint8_t> bytes;
    void        Put(std::initializer_list<std::uint8_t> b) { bytes.insert(bytes.end(), b); }
    void        Put(const std::uint8_t* b, std::size_t n) { bytes.insert(bytes.end(), b, b + n); }
    std::size_t At() const { return bytes.size(); }
    void        PadTo(std::size_t blockOffset)
    {
        while ((bytes.size() & 0xF) != blockOffset)
        {
            bytes.push_back(0x90);
        }
    }
};

/// Executable memory holding `bytes`, 16-byte aligned (a fresh page).
std::uint8_t* Load(const std::vector<std::uint8_t>& bytes)
{
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    std::memcpy(page, bytes.data(), bytes.size());
    return page;
}

// --- 1. The centred frame ------------------------------------------------------------------------------------------
// void Frame(float io[8]): xmm1 = io[0] Min.Y, xmm2 = io[1] Min.X, xmm3 = io[2] Max.X, xmm7 = io[3] Max.Y; rbx = io+4;
// stores Min as the game did before the site, then the site (block offset 4), then the game's last store of Max.X.
struct FrameStandIn
{
    using Fn = void (*)(float* io);
    Fn          fn   = nullptr;
    std::size_t site = 0;
    std::uint8_t* page = nullptr;

    FrameStandIn()
    {
        Builder b;
        b.Put({0x53});                          // push rbx
        b.Put({0x48, 0x83, 0xEC, 0x20});        // sub rsp, 20h
        b.Put({0x0F, 0x11, 0x3C, 0x24});        // movups [rsp], xmm7
        b.Put({0x48, 0x8D, 0x59, 0x10});        // lea rbx, [rcx+10h]
        b.Put({0xF3, 0x0F, 0x10, 0x09});        // movss xmm1, [rcx]
        b.Put({0xF3, 0x0F, 0x10, 0x51, 0x04});  // movss xmm2, [rcx+4]
        b.Put({0xF3, 0x0F, 0x10, 0x59, 0x08});  // movss xmm3, [rcx+8]
        b.Put({0xF3, 0x0F, 0x10, 0x79, 0x0C});  // movss xmm7, [rcx+0Ch]
        b.Put({0xF3, 0x0F, 0x11, 0x13});        // movss [rbx], xmm2      Min.X
        b.Put({0xF3, 0x0F, 0x11, 0x4B, 0x04});  // movss [rbx+4], xmm1    Min.Y
        b.PadTo(code::kFrameSite & 0xF);
        site = b.At();
        b.Put(code::kFrameStore, sizeof(code::kFrameStore));  // the site: movss [rbx+0Ch], xmm7
        b.Put({0xF3, 0x0F, 0x11, 0x5B, 0x08});  // movss [rbx+8], xmm3    Max.X (the game's last store)
        b.Put({0x0F, 0x10, 0x3C, 0x24});        // movups xmm7, [rsp]
        b.Put({0x48, 0x83, 0xC4, 0x20});        // add rsp, 20h
        b.Put({0x5B, 0xC3});                    // pop rbx; ret
        page = Load(b.bytes);
        fn   = reinterpret_cast<Fn>(page);
    }
};

void TestFrame()
{
    std::puts("centred frame: the whole layout while the band applies, the game's box otherwise");
    FrameStandIn             standIn;
    alignas(8) std::uint8_t  band = 0;
    lss::platform::CodeRedirect redirect;
    const auto               bytes = code::FrameCode(band);
    EXPECT(redirect.Install(reinterpret_cast<std::uintptr_t>(standIn.page + standIn.site), code::kFrameStore, bytes,
                            reinterpret_cast<std::uintptr_t>(standIn.page + standIn.site + sizeof(code::kFrameStore))));
    // A 3840x1080 layout: the game's box is (960,0)-(2880,1080).
    float io[8] = {0.0f, 960.0f, 2880.0f, 1080.0f};
    standIn.fn(io);
    EXPECT(io[4] == 960.0f && io[5] == 0.0f && io[6] == 2880.0f && io[7] == 1080.0f);
    band     = 1;
    float b2[8] = {0.0f, 960.0f, 2880.0f, 1080.0f};
    standIn.fn(b2);
    EXPECT(b2[4] == 0.0f && b2[5] == 0.0f && b2[6] == 3840.0f && b2[7] == 1080.0f);
    // A 1920x2160 layout (a tall band): the game's box is (0,540)-(1920,1620).
    float b3[8] = {540.0f, 0.0f, 1920.0f, 1620.0f};
    standIn.fn(b3);
    EXPECT(b3[4] == 0.0f && b3[5] == 0.0f && b3[6] == 1920.0f && b3[7] == 2160.0f);
    EXPECT(redirect.Restore());
}

// --- 2. The arrow's edge -------------------------------------------------------------------------------------------
// int Arrow(float io[24]): xmm0 = io[0] |angle|, xmm7 = io[1] (0), xmm8 = io[2] L.H-h, xmm9 = io[3], xmm10 = io[4]
// ySide, xmm12 = io[5]; [rsp+50h] = io[6..9], [rsp+70h] = io[10..13]; then the site (block offset 3, comparing with a
// 60.0 of its own). Returns 1 from its side label, 2 from its top/bottom label, 3 if the code fell through; writes
// xmm9, xmm12, xmm14 back to io[14..16].
struct ArrowStandIn
{
    using Fn = int (*)(float* io);
    Fn            fn = nullptr;
    std::size_t   site = 0, side = 0, topBottom = 0;
    std::uint8_t* page = nullptr;

    ArrowStandIn()
    {
        Builder b;
        b.Put({0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00});              // sub rsp, 0A8h (rsp 16-aligned)
        b.Put({0x48, 0x89, 0xCA});                                      // mov rdx, rcx (the code uses rcx)
        b.Put({0x0F, 0x11, 0x3C, 0x24});                                // movups [rsp], xmm7
        b.Put({0x44, 0x0F, 0x11, 0x44, 0x24, 0x10});                    // movups [rsp+10h], xmm8
        b.Put({0x44, 0x0F, 0x11, 0x4C, 0x24, 0x20});                    // movups [rsp+20h], xmm9
        b.Put({0x44, 0x0F, 0x11, 0x54, 0x24, 0x30});                    // movups [rsp+30h], xmm10
        b.Put({0x44, 0x0F, 0x11, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00});  // movups [rsp+80h], xmm12
        b.Put({0x44, 0x0F, 0x11, 0xB4, 0x24, 0x90, 0x00, 0x00, 0x00});  // movups [rsp+90h], xmm14
        b.Put({0xF3, 0x0F, 0x10, 0x02});                                // movss xmm0, [rdx]
        b.Put({0xF3, 0x0F, 0x10, 0x7A, 0x04});                          // movss xmm7, [rdx+4]
        b.Put({0xF3, 0x44, 0x0F, 0x10, 0x42, 0x08});                    // movss xmm8, [rdx+8]
        b.Put({0xF3, 0x44, 0x0F, 0x10, 0x4A, 0x0C});                    // movss xmm9, [rdx+0Ch]
        b.Put({0xF3, 0x44, 0x0F, 0x10, 0x52, 0x10});                    // movss xmm10, [rdx+10h]
        b.Put({0xF3, 0x44, 0x0F, 0x10, 0x62, 0x14});                    // movss xmm12, [rdx+14h]
        b.Put({0x0F, 0x10, 0x4A, 0x18});                                // movups xmm1, [rdx+18h]
        b.Put({0x0F, 0x11, 0x4C, 0x24, 0x50});                          // movups [rsp+50h], xmm1
        b.Put({0x0F, 0x10, 0x4A, 0x28});                                // movups xmm1, [rdx+28h]
        b.Put({0x0F, 0x11, 0x4C, 0x24, 0x70});                          // movups [rsp+70h], xmm1
        b.PadTo(code::kArrowSite & 0xF);
        site = b.At();
        b.Put({0x0F, 0x2F, 0x05, 0, 0, 0, 0});                          // the site: comiss xmm0, [rip+sixty] (fixed below)
        b.Put({0xB8, 0x03, 0x00, 0x00, 0x00});                          // mov eax, 3 (fell through)
        b.Put({0xEB, 0x0C});                                            // jmp epilogue
        side = b.At();
        b.Put({0xB8, 0x01, 0x00, 0x00, 0x00});                          // side: mov eax, 1
        b.Put({0xEB, 0x05});                                            // jmp epilogue
        topBottom = b.At();
        b.Put({0xB8, 0x02, 0x00, 0x00, 0x00});                          // topBottom: mov eax, 2
        b.Put({0xF3, 0x44, 0x0F, 0x11, 0x4A, 0x38});                    // epilogue: movss [rdx+38h], xmm9
        b.Put({0xF3, 0x44, 0x0F, 0x11, 0x62, 0x3C});                    // movss [rdx+3Ch], xmm12
        b.Put({0xF3, 0x44, 0x0F, 0x11, 0x72, 0x40});                    // movss [rdx+40h], xmm14
        b.Put({0x0F, 0x10, 0x3C, 0x24});                                // movups xmm7, [rsp]
        b.Put({0x44, 0x0F, 0x10, 0x44, 0x24, 0x10});                    // movups xmm8, [rsp+10h]
        b.Put({0x44, 0x0F, 0x10, 0x4C, 0x24, 0x20});                    // movups xmm9, [rsp+20h]
        b.Put({0x44, 0x0F, 0x10, 0x54, 0x24, 0x30});                    // movups xmm10, [rsp+30h]
        b.Put({0x44, 0x0F, 0x10, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00});  // movups xmm12, [rsp+80h]
        b.Put({0x44, 0x0F, 0x10, 0xB4, 0x24, 0x90, 0x00, 0x00, 0x00});  // movups xmm14, [rsp+90h]
        b.Put({0x48, 0x81, 0xC4, 0xA8, 0x00, 0x00, 0x00, 0xC3});        // add rsp, 0A8h; ret
        b.PadTo(0);
        const std::size_t sixty = b.At();
        const float       value = 60.0f;
        b.Put(reinterpret_cast<const std::uint8_t*>(&value), sizeof(value));
        const auto rel = static_cast<std::int32_t>(sixty - (site + 7));
        std::memcpy(&b.bytes[site + 3], &rel, sizeof(rel));
        page = Load(b.bytes);
        fn   = reinterpret_cast<Fn>(page);
    }
};

struct ArrowCase
{
    float angle, lMinusH, x, ySide, cx;
};

int RunArrow(ArrowStandIn& s, const ArrowCase& c, float& x, float& xmm12, float& xmm14)
{
    float io[24] = {};
    io[0] = c.angle;
    io[1] = 0.0f;
    io[2] = c.lMinusH;
    io[3] = c.x;
    io[4] = c.ySide;
    io[5] = c.cx;
    io[6] = 14.0f;   // [rsp+50h] -> xmm14
    io[10] = 12.0f;  // [rsp+70h] -> xmm12
    const int branch = s.fn(io);
    x     = io[14];
    xmm12 = io[15];
    xmm14 = io[16];
    return branch;
}

void TestArrow()
{
    std::puts("arrow edge: the game's 60/120 split with the flag clear, the ray's real edge with it set");
    ArrowStandIn             s;
    alignas(8) std::uint8_t  band = 0;
    lss::platform::CodeRedirect redirect;
    std::uint8_t             site[7];  // the stand-in's own compare, its displacement included
    std::memcpy(site, s.page + s.site, sizeof(site));
    const auto bytes = code::ArrowCode(band, reinterpret_cast<std::uintptr_t>(s.page + s.side),
                                       reinterpret_cast<std::uintptr_t>(s.page + s.topBottom), 60.0f, 120.0f);
    EXPECT(redirect.Install(reinterpret_cast<std::uintptr_t>(s.page + s.site), site, bytes,
                            reinterpret_cast<std::uintptr_t>(s.page + s.site + 7)));
    float x = 0, x12 = 0, x14 = 0;
    // The game's rule: side between 60 and 120 inclusive of neither end's top/bottom (jbe 60, jae 120).
    EXPECT(RunArrow(s, {30.0f, 1000.0f, 5.0f, -50.0f, 100.0f}, x, x12, x14) == 2);
    EXPECT(x == 105.0f && x12 == 12.0f && x14 == 14.0f);  // the three instructions ran: x += cx, both loads
    EXPECT(RunArrow(s, {60.0f, 1000.0f, 0.0f, 500.0f, 0.0f}, x, x12, x14) == 2);
    EXPECT(RunArrow(s, {61.0f, 1000.0f, 0.0f, 5000.0f, 0.0f}, x, x12, x14) == 1);  // ySide ignored by the game
    EXPECT(RunArrow(s, {119.0f, 1000.0f, 0.0f, -10.0f, 0.0f}, x, x12, x14) == 1);
    EXPECT(RunArrow(s, {120.0f, 1000.0f, 0.0f, 500.0f, 0.0f}, x, x12, x14) == 2);
    EXPECT(RunArrow(s, {std::nanf(""), 1000.0f, 0.0f, 500.0f, 0.0f}, x, x12, x14) == 2);  // unordered: jbe taken
    band = 1;
    // A 32:9 half (L.H - h = 1000): a ray at 65 degrees whose side point lies below the half leaves by the bottom.
    EXPECT(RunArrow(s, {65.0f, 1000.0f, 5.0f, 1200.0f, 100.0f}, x, x12, x14) == 2);
    EXPECT(x == 105.0f && x12 == 12.0f && x14 == 14.0f);
    EXPECT(RunArrow(s, {80.0f, 1000.0f, 0.0f, 900.0f, 0.0f}, x, x12, x14) == 1);   // within: side
    EXPECT(RunArrow(s, {40.0f, 1000.0f, 0.0f, 300.0f, 0.0f}, x, x12, x14) == 1);   // an 8:9 half: side below 60
    EXPECT(RunArrow(s, {100.0f, 1000.0f, 0.0f, -1.0f, 0.0f}, x, x12, x14) == 2);   // above the top
    EXPECT(RunArrow(s, {100.0f, 1000.0f, 0.0f, 0.0f, 0.0f}, x, x12, x14) == 1);    // on the corner: side
    EXPECT(RunArrow(s, {100.0f, 1000.0f, 0.0f, 1000.0f, 0.0f}, x, x12, x14) == 1); // on the corner: side
    EXPECT(RunArrow(s, {0.0f, 1000.0f, 0.0f, 500.0f, 0.0f}, x, x12, x14) == 2);    // vertical: ySide degenerates
    EXPECT(RunArrow(s, {179.99999f, 1000.0f, 0.0f, 500.0f, 0.0f}, x, x12, x14) == 2);
    EXPECT(RunArrow(s, {std::nanf(""), 1000.0f, 0.0f, 500.0f, 0.0f}, x, x12, x14) == 2);
    EXPECT(redirect.Restore());
}

// --- 3. The battle arrows' edge -------------------------------------------------------------------------------------
// int BattleArrow(float io[8]): xmm1 = io[0] |angle|, xmm6 = 0, xmm11 = io[1] L.H-h, xmm12 = io[2] the side point's y
// from the centre, [rsp+20h] = io[3] the centre's y; the game's 60° compare, then the site (block offset 7), the game's
// jbe to top/bottom, its 120° compare and jae, then the side branch. Returns 1 from its side label, 2 from its
// top/bottom label; writes xmm12 (ySide, made by the site) to io[4].
struct BattleArrowStandIn
{
    using Fn = int (*)(float* io);
    Fn            fn = nullptr;
    std::size_t   site = 0, side = 0, topBottom = 0;
    std::uint8_t* page = nullptr;

    BattleArrowStandIn()
    {
        Builder     b;
        const auto  rel8 = [&b](std::size_t field, std::size_t target) {
            b.bytes[field] = static_cast<std::uint8_t>(static_cast<std::int8_t>(target - (field + 1)));
        };
        b.Put({0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00});  // sub rsp, 0A8h (rsp 16-aligned)
        b.Put({0x48, 0x89, 0xCA});                          // mov rdx, rcx (the code uses rcx)
        b.Put({0x0F, 0x11, 0x74, 0x24, 0x30});              // movups [rsp+30h], xmm6
        b.Put({0x44, 0x0F, 0x11, 0x5C, 0x24, 0x40});        // movups [rsp+40h], xmm11
        b.Put({0x44, 0x0F, 0x11, 0x64, 0x24, 0x50});        // movups [rsp+50h], xmm12
        b.Put({0xF3, 0x0F, 0x10, 0x0A});                    // movss xmm1, [rdx]
        b.Put({0x0F, 0x57, 0xF6});                          // xorps xmm6, xmm6
        b.Put({0xF3, 0x44, 0x0F, 0x10, 0x5A, 0x04});        // movss xmm11, [rdx+4]
        b.Put({0xF3, 0x44, 0x0F, 0x10, 0x62, 0x08});        // movss xmm12, [rdx+8]
        b.Put({0xF3, 0x0F, 0x10, 0x42, 0x0C});              // movss xmm0, [rdx+0Ch]
        b.Put({0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20});        // movss [rsp+20h], xmm0
        const std::size_t sixtyCompare = b.At();
        b.Put({0x0F, 0x2F, 0x0D, 0, 0, 0, 0});              // comiss xmm1, [rip+sixty] (fixed below)
        b.PadTo(code::kBattleArrowSite & 0xF);              // nops: the flags stay the compare's
        site = b.At();
        b.Put(code::kBattleArrowAdd, sizeof(code::kBattleArrowAdd));  // the site: addss xmm12, [rsp+20h]
        const std::size_t jbe = b.At();
        b.Put({0x76, 0x00});                                // jbe topBottom
        const std::size_t oneTwentyCompare = b.At();
        b.Put({0x0F, 0x2F, 0x0D, 0, 0, 0, 0});              // comiss xmm1, [rip+oneTwenty]
        const std::size_t jae = b.At();
        b.Put({0x73, 0x00});                                // jae topBottom
        side = b.At();
        b.Put({0xB8, 0x01, 0x00, 0x00, 0x00});              // side: mov eax, 1
        const std::size_t toEpilogue = b.At();
        b.Put({0xEB, 0x00});                                // jmp epilogue
        topBottom = b.At();
        b.Put({0xB8, 0x02, 0x00, 0x00, 0x00});              // topBottom: mov eax, 2
        const std::size_t epilogue = b.At();
        b.Put({0xF3, 0x44, 0x0F, 0x11, 0x62, 0x10});        // movss [rdx+10h], xmm12
        b.Put({0x0F, 0x10, 0x74, 0x24, 0x30});              // movups xmm6, [rsp+30h]
        b.Put({0x44, 0x0F, 0x10, 0x5C, 0x24, 0x40});        // movups xmm11, [rsp+40h]
        b.Put({0x44, 0x0F, 0x10, 0x64, 0x24, 0x50});        // movups xmm12, [rsp+50h]
        b.Put({0x48, 0x81, 0xC4, 0xA8, 0x00, 0x00, 0x00, 0xC3});  // add rsp, 0A8h; ret
        b.PadTo(0);
        const std::size_t constants = b.At();
        const float       values[2] = {60.0f, 120.0f};
        b.Put(reinterpret_cast<const std::uint8_t*>(values), sizeof(values));
        rel8(jbe + 1, topBottom);
        rel8(jae + 1, topBottom);
        rel8(toEpilogue + 1, epilogue);
        const auto disp = [&b](std::size_t compare, std::size_t target) {
            const auto rel = static_cast<std::int32_t>(target - (compare + 7));
            std::memcpy(&b.bytes[compare + 3], &rel, sizeof(rel));
        };
        disp(sixtyCompare, constants);
        disp(oneTwentyCompare, constants + 4);
        page = Load(b.bytes);
        fn   = reinterpret_cast<Fn>(page);
    }
};

int RunBattleArrow(BattleArrowStandIn& s, float angle, float lMinusH, float yFromCentre, float cy, float& ySide)
{
    float io[8] = {angle, lMinusH, yFromCentre, cy};
    const int branch = s.fn(io);
    ySide = io[4];
    return branch;
}

void TestBattleArrow()
{
    std::puts("battle arrow edge: the game's 60/120 split with the flag clear, the ray's real edge with it set");
    BattleArrowStandIn          s;
    alignas(8) std::uint8_t     band = 0;
    lss::platform::CodeRedirect redirect;
    const auto bytes = code::BattleArrowCode(band, reinterpret_cast<std::uintptr_t>(s.page + s.side),
                                             reinterpret_cast<std::uintptr_t>(s.page + s.topBottom), 60.0f);
    EXPECT(redirect.Install(reinterpret_cast<std::uintptr_t>(s.page + s.site), code::kBattleArrowAdd, bytes,
                            reinterpret_cast<std::uintptr_t>(s.page + s.site + sizeof(code::kBattleArrowAdd))));
    float y = 0;
    // The game's rule (L.H - h = 1000, centre 500): top/bottom at 60 or less and at 120 or more, whatever ySide says.
    EXPECT(RunBattleArrow(s, 30.0f, 1000.0f, -550.0f, 500.0f, y) == 2);
    EXPECT(y == -50.0f);  // the replaced add ran
    EXPECT(RunBattleArrow(s, 60.0f, 1000.0f, 0.0f, 500.0f, y) == 2);
    EXPECT(RunBattleArrow(s, 61.0f, 1000.0f, 4500.0f, 500.0f, y) == 1);  // ySide ignored by the game
    EXPECT(y == 5000.0f);
    EXPECT(RunBattleArrow(s, 119.0f, 1000.0f, -510.0f, 500.0f, y) == 1);
    EXPECT(RunBattleArrow(s, 120.0f, 1000.0f, 0.0f, 500.0f, y) == 2);
    EXPECT(RunBattleArrow(s, std::nanf(""), 1000.0f, 0.0f, 500.0f, y) == 2);  // unordered: jbe taken
    band = 1;
    // A wide band: a ray at 65 degrees whose side point lies below the band leaves by the bottom.
    EXPECT(RunBattleArrow(s, 65.0f, 1000.0f, 700.0f, 500.0f, y) == 2);
    EXPECT(y == 1200.0f);
    EXPECT(RunBattleArrow(s, 80.0f, 1000.0f, 400.0f, 500.0f, y) == 1);    // within: side
    EXPECT(y == 900.0f);
    EXPECT(RunBattleArrow(s, 40.0f, 1000.0f, -200.0f, 500.0f, y) == 1);   // a narrow column: side below 60
    EXPECT(RunBattleArrow(s, 100.0f, 1000.0f, -501.0f, 500.0f, y) == 2);  // above the top
    EXPECT(RunBattleArrow(s, 100.0f, 1000.0f, -500.0f, 500.0f, y) == 1);  // on the corner: side
    EXPECT(RunBattleArrow(s, 100.0f, 1000.0f, 500.0f, 500.0f, y) == 1);   // on the corner: side
    EXPECT(RunBattleArrow(s, 150.0f, 1000.0f, 300.0f, 500.0f, y) == 1);   // past 120 but within: side
    EXPECT(RunBattleArrow(s, 0.0f, 1000.0f, 0.0f, 500.0f, y) == 2);       // vertical: ySide degenerates to the centre
    EXPECT(RunBattleArrow(s, 179.99999f, 1000.0f, 0.0f, 500.0f, y) == 2);
    EXPECT(RunBattleArrow(s, std::nanf(""), 1000.0f, 0.0f, 500.0f, y) == 2);
    band = 0;  // back to the game's
    EXPECT(RunBattleArrow(s, 40.0f, 1000.0f, -200.0f, 500.0f, y) == 2);
    EXPECT(redirect.Restore());
}

}  // namespace

// --- 4. The layout scale -------------------------------------------------------------------------------------------
// void Scale(float* io): xmm2 = io[0] height, edx = io[1] (an int) width; the two replaced instructions at the game's block
// offset, the game's multiply by 1/1080 of its own, the game's conversion of the width; writes the scale and the width back.
struct ScaleStandIn
{
    using Fn = void (*)(void* io);
    Fn            fn     = nullptr;
    std::size_t   site   = 0;
    std::size_t   resume = 0;
    std::uint8_t* page   = nullptr;

    ScaleStandIn()
    {
        Builder b;
        b.Put({0xF3, 0x0F, 0x10, 0x11});  // movss xmm2, [rcx]
        b.Put({0x8B, 0x51, 0x04});        // mov edx, [rcx+4]
        b.PadTo(code::kScaleSite & 0xF);
        site = b.At();
        b.Put(code::kScaleReplaced, sizeof(code::kScaleReplaced));
        const std::size_t multiply = b.At();
        b.Put({0xF3, 0x0F, 0x59, 0x25, 0, 0, 0, 0});  // mulss xmm4, [rip+constant], patched below
        resume = b.At();
        b.Put({0x0F, 0x5B, 0xDB});              // cvtdq2ps xmm3, xmm3
        b.Put({0xF3, 0x0F, 0x11, 0x61, 0x08});  // movss [rcx+8], xmm4
        b.Put({0xF3, 0x0F, 0x11, 0x59, 0x0C});  // movss [rcx+0Ch], xmm3
        b.Put({0xC3});                          // ret
        const std::size_t constant = b.At();
        const float       k1080    = 1.0f / 1080.0f;
        b.Put(reinterpret_cast<const std::uint8_t*>(&k1080), sizeof(k1080));
        const auto rel = static_cast<std::int32_t>(constant - resume);
        std::memcpy(&b.bytes[multiply + 4], &rel, sizeof(rel));
        page = Load(b.bytes);
        fn   = reinterpret_cast<Fn>(page);
    }
};

struct ScaleIo
{
    float        height;
    std::int32_t width;
    float        scale;
    float        widthOut;
};

ScaleIo RunScale(const ScaleStandIn& standIn, int width, int height)
{
    ScaleIo io{static_cast<float>(height), width, 0.0f, 0.0f};
    standIn.fn(&io);
    return io;
}

void TestScale()
{
    std::puts("layout scale: the contain rule, the game's value for 16:9 and wider");
    ScaleStandIn standIn;
    const auto   close = [](float a, float b) { return std::fabs(a - b) < 0.0001f; };
    // Without the code: height/1080 whatever the width, the part that misplaces markers in a tall band.
    EXPECT(close(RunScale(standIn, 1720, 1440).scale, 1440.0f / 1080.0f));
    // A rect of 16:9 or wider keeps the game's value to the last bit, which is why the code needs no flag: single player runs it.
    struct Size
    {
        int width, height;
        float game;
    };
    Size sizes[] = {{1920, 1080, 0.0f}, {2560, 1440, 0.0f}, {3840, 2160, 0.0f}, {3840, 1080, 0.0f}, {1280, 720, 0.0f}, {5120, 1440, 0.0f}};
    for (Size& size : sizes)
    {
        size.game = RunScale(standIn, size.width, size.height).scale;
    }
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(reinterpret_cast<std::uintptr_t>(standIn.page + standIn.site), code::kScaleReplaced, code::kScaleCode,
                            reinterpret_cast<std::uintptr_t>(standIn.page + standIn.resume)));
    for (const Size& size : sizes)
    {
        EXPECT(RunScale(standIn, size.width, size.height).scale == size.game);
    }
    EXPECT(close(RunScale(standIn, 1920, 1080).scale, 1.0f));              // 16:9: the game's
    EXPECT(close(RunScale(standIn, 2560, 1440).scale, 1440.0f / 1080.0f));  // 16:9: the game's
    EXPECT(close(RunScale(standIn, 3840, 1080).scale, 1.0f));              // a top and bottom band: the game's
    EXPECT(close(RunScale(standIn, 960, 1080).scale, 0.5f));               // side by side: width/1920
    EXPECT(close(RunScale(standIn, 1720, 1440).scale, 1720.0f / 1920.0f));  // side by side on an ultrawide's band
    // The width still reaches the game's own conversion.
    EXPECT(RunScale(standIn, 1720, 1440).widthOut == 1720.0f);
    EXPECT(redirect.Restore());
    EXPECT(close(RunScale(standIn, 1720, 1440).scale, 1440.0f / 1080.0f));
}

int main()
{
    TestFrame();
    TestArrow();
    TestBattleArrow();
    TestScale();
    if (g_failures == 0)
    {
        std::puts("all tests passed");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
