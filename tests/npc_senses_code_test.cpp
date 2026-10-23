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

/// The two-player rule as it stood before the N form (v0.29.0 .. v0.58.0), kept verbatim as the reference.
code::Choice OldChooseCentre(code::Cell one, code::Cell two) noexcept
{
    if (std::abs(two.z - one.z) > 1)
    {
        return {code::Fit::OtherFloor, one};
    }
    bool       tooFar = false;
    const auto axis   = [&tooFar](int a, int b) {
        const int d = b - a;
        if (d > 2 * code::kReach || d < -2 * code::kReach)
        {
            tooFar = true;
        }
        return d > code::kReach ? b - code::kReach : d < -code::kReach ? b + code::kReach : a;
    };
    const code::Cell centre{axis(one.x, two.x), axis(one.y, two.y), one.z};
    if (tooFar)
    {
        return {code::Fit::TooFar, one};
    }
    return {centre == one ? code::Fit::Inside : code::Fit::Shifted, centre};
}

void TestChooseCentreThree()
{
    std::puts("centre with three players: moved the least that reaches every player it can hold, player 1 first, then "
              "player 2 before player 3; a player too far from those held is left out");
    const code::Cell one{10, 20, 3};
    auto choose = [&](code::Cell two, code::Cell three) {
        const code::Cell others[] = {two, three};
        return code::ChooseCentre(one, others, 2);
    };

    auto c = choose({12, 20, 3}, {14, 22, 3});  // both inside
    EXPECT(c.fit == code::Fit::Inside && c.centre == one && c.fits[0] == code::Fit::Inside && c.fits[1] == code::Fit::Inside);
    c = choose({12, 20, 3}, {18, 20, 3});  // player 3 eight cells out: the centre moves two, player 2 still inside
    EXPECT(c.fit == code::Fit::Shifted && (c.centre == code::Cell{12, 20, 3}) && c.fits[0] == code::Fit::Inside &&
           c.fits[1] == code::Fit::Shifted);
    c = choose({18, 20, 3}, {2, 20, 3});  // players 2 and 3 on opposite sides, 16 apart: player 2 held (the lower seat), player 3 left out
    EXPECT(c.fit == code::Fit::Shifted && (c.centre == code::Cell{12, 20, 3}) && c.fits[0] == code::Fit::Shifted &&
           c.fits[1] == code::Fit::TooFar);
    c = choose({18, 20, 3}, {4, 20, 3});  // 14 apart across the players: still more than two reaches, player 3 out
    EXPECT(c.fit == code::Fit::Shifted && (c.centre == code::Cell{12, 20, 3}) && c.fits[1] == code::Fit::TooFar);
    c = choose({16, 20, 3}, {4, 20, 3});  // 12 apart: one grid holds all three, the centre at 10 (player 1's own cell)
    EXPECT(c.fit == code::Fit::Inside && c.centre == one && c.fits[0] == code::Fit::Inside && c.fits[1] == code::Fit::Inside);
    c = choose({17, 20, 3}, {5, 20, 3});  // 12 apart but player 1 off their middle: the centre moves one, both reached
    EXPECT(c.fit == code::Fit::Shifted && (c.centre == code::Cell{11, 20, 3}) && c.fits[0] == code::Fit::Shifted &&
           c.fits[1] == code::Fit::Inside);
    c = choose({12, 20, 5}, {17, 20, 3});  // player 2 two floors up: left out, player 3 moves the centre
    EXPECT(c.fit == code::Fit::Shifted && (c.centre == code::Cell{11, 20, 3}) && c.fits[0] == code::Fit::OtherFloor &&
           c.fits[1] == code::Fit::Shifted);
    c = choose({30, 20, 3}, {12, 20, 5});  // nobody held: the first left-out player's reason
    EXPECT(c.fit == code::Fit::TooFar && c.centre == one && c.fits[0] == code::Fit::TooFar && c.fits[1] == code::Fit::OtherFloor);
    EXPECT(code::ChooseCentre(one, nullptr, 0).fit == code::Fit::Inside && code::ChooseCentre(one, nullptr, 0).centre == one);

    // The N form with one further player is the old two-cell rule (a verbatim copy, above) on every input: every cell
    // within 15 either way, two floors up and down.
    for (int dz = -2; dz <= 2; ++dz)
    {
        for (int dx = -15; dx <= 15; ++dx)
        {
            for (int dy = -15; dy <= 15; ++dy)
            {
                const code::Cell two{one.x + dx, one.y + dy, one.z + dz};
                const auto       a = OldChooseCentre(one, two);
                const auto       b = code::ChooseCentre(one, &two, 1);
                EXPECT(a.fit == b.fit && a.centre == b.centre && b.fits[0] == a.fit);
            }
        }
    }
    // Whatever the choice with three players: player 1 always within reach of the centre; every held player too; and
    // the centre moved the least (a step back toward player 1's cell on either axis would lose a held player).
    const auto held = [](code::Fit fit) { return fit == code::Fit::Inside || fit == code::Fit::Shifted; };
    const auto reaches = [](code::Cell centre, code::Cell cell) {
        return std::abs(centre.x - cell.x) <= code::kReach && std::abs(centre.y - cell.y) <= code::kReach;
    };
    for (int dx = -15; dx <= 15; ++dx)
    {
        for (int dy = -15; dy <= 15; ++dy)
        {
            const code::Cell two{one.x + dx, one.y + dy, one.z};
            const code::Cell three{one.x - dy, one.y + dx, one.z};
            const auto       r = choose(two, three);
            EXPECT(reaches(r.centre, one));
            EXPECT(!held(r.fits[0]) || reaches(r.centre, two));
            EXPECT(!held(r.fits[1]) || reaches(r.centre, three));
            for (const int axis : {0, 1})
            {
                code::Cell back = r.centre;
                int&       v    = axis == 0 ? back.x : back.y;
                const int  own  = axis == 0 ? one.x : one.y;
                if (v == own)
                {
                    continue;
                }
                v += v > own ? -1 : 1;
                const bool stillAll = (!held(r.fits[0]) || reaches(back, two)) && (!held(r.fits[1]) || reaches(back, three));
                EXPECT(!stillAll);
            }
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
