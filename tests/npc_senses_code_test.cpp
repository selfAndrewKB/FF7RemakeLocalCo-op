// Host-side tests for world/npc_senses_code.h, run without the game: the cell rule, the centre choice, and the grid
// code installed through platform::CodeRedirect over a stand-in that holds the replaced instruction at the game's
// block offset, and EXECUTED.

#include "platform/code_redirect.h"
#include "world/npc_senses_code.h"

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

namespace code = lss::world::npc_senses_code;

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

void TestCellRule()
{
    std::puts("cell rule: the game's rounding (4 m cells centred on multiples of 400)");
    // Away from the half-way ties the rule is floor((v + 200) / 400).
    for (float v : {0.0f, 150.0f, 199.0f, 201.0f, 599.0f, 601.0f, -150.0f, -201.0f, -599.0f, -601.0f, 16793.0f,
                    -1153.0f, 13893.0f, -2466.0f, 978.0f, 1307.0f})
    {
        EXPECT(code::CellOf(v) == static_cast<int>(std::floor((v + 200.0f) / 400.0f)));
    }
    // Read live 2026-10-21: player 1 at (-851, 16932, 1163) in the grid centred (-2, 42, 3).
    const float one[3] = {-851.0f, 16932.0f, 1163.0f};
    EXPECT((code::CellOf(one) == code::Cell{-2, 42, 3}));
}

void TestChooseCentre()
{
    std::puts("centre: the game's while player 2 is within reach; moved toward player 2 at most one reach; never "
              "when too far apart or on another floor");
    const code::Cell one{10, 20, 3};
    auto             choose = [&](code::Cell two) { return code::ChooseCentre(one, two); };

    auto c = choose({10, 20, 3});
    EXPECT(c.fit == code::Fit::Inside && c.centre == one);
    c = choose({16, 14, 4});  // six cells each way, one up: still inside
    EXPECT(c.fit == code::Fit::Inside && c.centre == one);
    c = choose({17, 20, 3});  // seven cells in x: the centre moves one
    EXPECT(c.fit == code::Fit::Shifted && (c.centre == code::Cell{11, 20, 3}));
    c = choose({10, 8, 2});  // twelve cells down in y, one floor down: both at the reach
    EXPECT(c.fit == code::Fit::Shifted && (c.centre == code::Cell{10, 14, 3}));
    c = choose({-2, 32, 3});  // twelve both ways
    EXPECT(c.fit == code::Fit::Shifted && (c.centre == code::Cell{4, 26, 3}));
    c = choose({23, 20, 3});  // thirteen cells: too far, player 1 first
    EXPECT(c.fit == code::Fit::TooFar && c.centre == one);
    c = choose({17, 33, 3});  // one axis shiftable, the other too far
    EXPECT(c.fit == code::Fit::TooFar && c.centre == one);
    c = choose({12, 20, 5});  // two floors up
    EXPECT(c.fit == code::Fit::OtherFloor && c.centre == one);
    c = choose({30, 20, 1});  // too far and another floor: the floor is reported
    EXPECT(c.fit == code::Fit::OtherFloor && c.centre == one);

    // Whatever the choice, both players are within reach of the centre when it moved.
    for (int dx = -12; dx <= 12; ++dx)
    {
        for (int dy = -12; dy <= 12; ++dy)
        {
            const code::Cell two{one.x + dx, one.y + dy, one.z};
            const auto       r = choose(two);
            EXPECT(std::abs(r.centre.x - one.x) <= code::kReach && std::abs(r.centre.y - one.y) <= code::kReach);
            EXPECT(std::abs(r.centre.x - two.x) <= code::kReach && std::abs(r.centre.y - two.y) <= code::kReach);
        }
    }
}

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

// void Centre(std::uint8_t* manager, void* unused, float* out): rbx = manager; after the site *out = xmm2.
struct CentreStandIn
{
    using Fn = void (*)(std::uint8_t* manager, void* unused, float* out);
    Fn            fn   = nullptr;
    std::size_t   site = 0;
    std::uint8_t* page = nullptr;

    CentreStandIn()
    {
        Builder b;
        b.Put({0x53});              // push rbx
        b.Put({0x48, 0x89, 0xCB});  // mov rbx, rcx
        b.PadTo(code::kCentreSite & 0xF);
        site = b.At();
        b.Put(code::kCentreLoad, sizeof(code::kCentreLoad));  // the site (never run as is: replaced first)
        b.Put({0xF3, 0x41, 0x0F, 0x11, 0x10});               // movss [r8], xmm2
        b.Put({0x5B, 0xC3});                                 // pop rbx; ret
        page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        std::memcpy(page, b.bytes.data(), b.bytes.size());
        fn = reinterpret_cast<Fn>(page);
    }
};

void TestCentreCode()
{
    std::puts("grid code: the game's centre kept while off, the mod's three cells written while on; xmm2 = 400.0f");
    CentreStandIn               s;
    alignas(16) code::CentreState state;
    alignas(16) static float    cellSize = 400.0f;
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(reinterpret_cast<std::uintptr_t>(s.page + s.site), code::kCentreLoad,
                            code::CentreCode(state, reinterpret_cast<std::uintptr_t>(&cellSize)),
                            reinterpret_cast<std::uintptr_t>(s.page + s.site + sizeof(code::kCentreLoad))));
    std::vector<std::uint8_t> manager(0x16800, 0xCC);
    const auto                cells = [&] {
        std::int32_t c[3];
        std::memcpy(c, manager.data() + code::kManagerCentre, sizeof(c));
        return code::Cell{c[0], c[1], c[2]};
    };
    const std::int32_t game[3] = {-2, 42, 3};
    std::memcpy(manager.data() + code::kManagerCentre, game, sizeof(game));
    float out = 0.0f;
    s.fn(manager.data(), nullptr, &out);
    EXPECT((cells() == code::Cell{-2, 42, 3}) && out == 400.0f && state.writes == 0);  // off: the game's
    state.x = -1;
    state.y = 37;
    state.z = 3;
    state.on = 1;
    out = 0.0f;
    s.fn(manager.data(), nullptr, &out);
    EXPECT((cells() == code::Cell{-1, 37, 3}) && out == 400.0f && state.writes == 1);  // on: the mod's
    EXPECT(manager[code::kManagerCentre - 1] == 0xCC && manager[code::kManagerCentre + 12] == 0xCC);  // nothing else
    state.on = 0;
    std::memcpy(manager.data() + code::kManagerCentre, game, sizeof(game));
    s.fn(manager.data(), nullptr, &out);
    EXPECT((cells() == code::Cell{-2, 42, 3}) && state.writes == 1);  // off again
    EXPECT(redirect.Restore());
}

}  // namespace

int main()
{
    TestCellRule();
    TestChooseCentre();
    TestCentreCode();
    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("all passed");
    return 0;
}
