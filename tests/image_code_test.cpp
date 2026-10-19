// Host-side tests for hud/image_code.h, run without the game: each code is installed through platform::CodeRedirect
// over a stand-in that holds the replaced instruction at the game's block offset, and EXECUTED.

#include "hud/image_code.h"
#include "platform/code_redirect.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

namespace code = lss::hud::image_code;

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

std::uint8_t* Load(const std::vector<std::uint8_t>& bytes)
{
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    std::memcpy(page, bytes.data(), bytes.size());
    return page;
}

// --- 1. Per-entry buffers ------------------------------------------------------------------------------------------
// std::uint64_t Buffers(std::uint64_t* io): rbp = io (so [rbp+150h] is io[42], the pool's VB), r9 = io[0] (the
// entry), [rsp+28h] = io[1] (the pool's IB, stored by the game before the site); after the site io[2] = [rsp+28h];
// returns rax (the VB argument).
struct BuffersStandIn
{
    using Fn = std::uint64_t (*)(std::uint64_t* io);
    Fn            fn   = nullptr;
    std::size_t   site = 0;
    std::uint8_t* page = nullptr;

    BuffersStandIn()
    {
        Builder b;
        b.Put({0x55});                                // push rbp
        b.Put({0x48, 0x83, 0xEC, 0x40});              // sub rsp, 40h (rsp 16-aligned)
        b.Put({0x48, 0x89, 0xCD});                    // mov rbp, rcx
        b.Put({0x4C, 0x8B, 0x09});                    // mov r9, [rcx]
        b.Put({0x48, 0x8B, 0x41, 0x08});              // mov rax, [rcx+8]
        b.Put({0x48, 0x89, 0x44, 0x24, 0x28});        // mov [rsp+28h], rax
        b.PadTo(code::kBuffersSite & 0xF);
        site = b.At();
        b.Put(code::kBuffersLoad, sizeof(code::kBuffersLoad));  // the site: mov rax, [rbp+150h]
        b.Put({0x48, 0x8B, 0x4C, 0x24, 0x28});        // mov rcx, [rsp+28h]
        b.Put({0x48, 0x89, 0x4D, 0x10});              // mov [rbp+10h], rcx   io[2]
        b.Put({0x48, 0x83, 0xC4, 0x40, 0x5D, 0xC3});  // add rsp, 40h; pop rbp; ret
        page = Load(b.bytes);
        fn   = reinterpret_cast<Fn>(page);
    }
};

void TestBuffers()
{
    std::puts("per-entry buffers: player 2's entries get player 2's pair, every other entry the pool's");
    BuffersStandIn              s;
    alignas(16) code::EntryTable table;
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(reinterpret_cast<std::uintptr_t>(s.page + s.site), code::kBuffersLoad, code::BuffersCode(table),
                            reinterpret_cast<std::uintptr_t>(s.page + s.site + sizeof(code::kBuffersLoad))));
    table.vertexBuffer = 0xB2B2;
    table.indexBuffer  = 0xC2C2;
    std::uint64_t io[64] = {};
    io[42]               = 0xB1B1;  // the pool's VB at [rbp+150h]
    const auto run       = [&](std::uint64_t entry) {
        io[0] = entry;
        io[1] = 0xC1C1;  // the pool's IB, already in the argument slot
        io[2] = 0;
        return s.fn(io);
    };
    EXPECT(run(0x1000) == 0xB1B1 && io[2] == 0xC1C1);  // no player 2 entry yet
    table.entries[0] = 0x2000;
    table.entries[3] = 0x2300;
    EXPECT(run(0x1000) == 0xB1B1 && io[2] == 0xC1C1);  // player 1's entry
    EXPECT(run(0x2000) == 0xB2B2 && io[2] == 0xC2C2);  // player 2's, first slot
    EXPECT(run(0x2300) == 0xB2B2 && io[2] == 0xC2C2);  // player 2's, last slot
    table.entries[0] = 0;                              // a slot cleared (player 2's image off)
    EXPECT(run(0x2000) == 0xB1B1 && io[2] == 0xC1C1);  // that entry is drawn with the pool's pair again
    EXPECT(redirect.Restore());
}

// --- 2. The navi map's pool 0 --------------------------------------------------------------------------------------
// int Navi(float* io): rbx = io; rsi = the qword at io+16 (the painting octagon); xmm8 = 0; the site (block offset 9)
// calls "acquire" (returns 7 in eax); the
// instruction after the call stores eax at io[0] and returns 1; the skip point stores xmm8 at io[1] and returns 2.
struct NaviStandIn
{
    using Fn = int (*)(float* io);
    Fn            fn = nullptr;
    std::size_t   site = 0, acquire = 0, skipTo = 0;
    std::uint8_t* page = nullptr;

    NaviStandIn()
    {
        Builder b;
        b.Put({0x53});                                      // push rbx
        b.Put({0x56});                                      // push rsi
        b.Put({0x48, 0x83, 0xEC, 0x38});                    // sub rsp, 38h (16-aligned again)
        b.Put({0x44, 0x0F, 0x11, 0x44, 0x24, 0x10});        // movups [rsp+10h], xmm8
        b.Put({0x48, 0x89, 0xCB});                          // mov rbx, rcx
        b.Put({0x48, 0x8B, 0x71, 0x10});                    // mov rsi, [rcx+10h]
        b.Put({0x45, 0x0F, 0x57, 0xC0});                    // xorps xmm8, xmm8
        b.PadTo(code::kNaviSite & 0xF);
        site = b.At();
        b.Put({0xE8, 0, 0, 0, 0});                          // the site: call acquire (fixed below)
        b.Put({0x89, 0x03});                                // mov [rbx], eax
        b.Put({0xB8, 0x01, 0x00, 0x00, 0x00});              // mov eax, 1
        b.Put({0xEB, 0x0B});                                // jmp epilogue
        skipTo = b.At();
        b.Put({0xF3, 0x44, 0x0F, 0x11, 0x43, 0x04});        // skip: movss [rbx+4], xmm8
        b.Put({0xB8, 0x02, 0x00, 0x00, 0x00});              // mov eax, 2
        b.Put({0x44, 0x0F, 0x10, 0x44, 0x24, 0x10});        // epilogue: movups xmm8, [rsp+10h]
        b.Put({0x48, 0x83, 0xC4, 0x38, 0x5E, 0x5B, 0xC3});  // add rsp, 38h; pop rsi; pop rbx; ret
        b.PadTo(0);
        acquire = b.At();
        b.Put({0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3});        // acquire: mov eax, 7; ret
        const auto rel = static_cast<std::int32_t>(acquire - (site + 5));
        std::memcpy(&b.bytes[site + 1], &rel, sizeof(rel));
        page = Load(b.bytes);
        fn   = reinterpret_cast<Fn>(page);
    }
};

void TestNavi()
{
    std::puts("navi map: the call made as the game makes it, or the named octagon's pool 0 span skipped with xmm8 = 1.0");
    NaviStandIn                 s;
    code::NaviState             state;
    lss::platform::CodeRedirect redirect;
    std::uint8_t                call[5];
    std::memcpy(call, s.page + s.site, sizeof(call));
    const auto at = [&s](std::size_t offset) { return reinterpret_cast<std::uintptr_t>(s.page + offset); };
    EXPECT(redirect.Install(at(s.site), call, code::NaviCode(state, at(s.site + 5), at(s.acquire), at(s.skipTo)),
                            at(s.site + 5)));
    alignas(8) float io[6] = {};
    const std::uintptr_t octagon = 0x5000;
    std::memcpy(&io[4], &octagon, sizeof(octagon));
    EXPECT(s.fn(io) == 1);  // nothing named: the call
    int acquired = 0;
    std::memcpy(&acquired, &io[0], sizeof(acquired));
    EXPECT(acquired == 7 && io[1] == 0.0f && state.seen == octagon && state.skips == 0);
    state.skip = 0x6000;  // another octagon named: this one makes the call
    io[0] = io[1] = 0.0f;
    EXPECT(s.fn(io) == 1 && io[1] == 0.0f);
    state.skip = octagon;  // this one named: skipped
    io[0] = io[1] = 0.0f;
    EXPECT(s.fn(io) == 2 && io[1] == 1.0f && io[0] == 0.0f && state.skips == 1);
    state.skip = 0;
    state.seen = 0;
    EXPECT(s.fn(io) == 1 && state.seen == octagon && state.skips == 1);  // the emulated call left the stack as it was
    EXPECT(redirect.Restore());
}

}  // namespace

int main()
{
    TestBuffers();
    TestNavi();
    if (g_failures == 0)
    {
        std::puts("all tests passed");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
