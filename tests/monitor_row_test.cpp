// Host-side tests for views::monitor_row::Choose, run without the game.
//
// The cases: the user's desktop of 2026-10-21 (an ultrawide primary above, holding the game; two equal 1440p
// monitors below it: the row is not the game's); Rebirth's three-monitor row (the game in it; a two-column span
// starting at the game's monitor and shifted left at the row's end); the row broken by a monitor of another size,
// a misaligned top (the user's pair 2 px apart), a gap, or Windows scaling; the game's monitor scaled with and
// without another row; one column always possible; bad inputs.

#include "views/monitor_row.h"

#include <cstdio>
#include <vector>

namespace {

using lss::views::monitor_row::Choice;
using lss::views::monitor_row::Choose;
using lss::views::monitor_row::Monitor;
using lss::views::monitor_row::Rect;

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

Monitor At(std::int32_t left, std::int32_t top, std::int32_t width = 2560, std::int32_t height = 1440,
           std::uint32_t dpi = 96)
{
    return {{left, top, left + width, top + height}, dpi};
}

/// The user's monitors on 2026-10-21 with the pair aligned: the ultrawide primary above (the game on it), the two
/// 1440p monitors in a row below.
std::vector<Monitor> UsersDesktop()
{
    return {At(0, 0, 3440, 1440), At(873, 1440), At(3433, 1440)};
}

/// Rebirth's row of three (the TV at 1440p at the left), the ultrawide above.
std::vector<Monitor> RowOfThree()
{
    return {At(0, 0, 3440, 1440), At(-1971, 1440), At(589, 1440), At(3149, 1440)};
}

void TestRowBelowTheGame()
{
    std::printf("the game on the ultrawide: the pair below is the row, from its left\n");
    const Choice c = Choose(UsersDesktop(), 0, 2);
    EXPECT(c.ok);
    EXPECT(c.rowMonitors == 2);
    EXPECT(!c.gamesRow);
    EXPECT((c.span == Rect{873, 1440, 5993, 2880}));
}

void TestUsersPairTwoPixelsOff()
{
    std::printf("the user's pair as first set up (the right one 2 px lower) is refused, naming the offset\n");
    std::vector<Monitor> monitors{At(0, 0, 3440, 1440), At(873, 1440), At(3433, 1442)};
    const Choice         c = Choose(monitors, 0, 2);
    EXPECT(!c.ok);
    EXPECT(c.rowMonitors == 1);
    EXPECT(c.why.find(L"2 px") != std::wstring::npos);
}

void TestThreeAcrossTheRow()
{
    std::printf("three columns across a row of three, from any of its monitors or from the ultrawide\n");
    const auto monitors = RowOfThree();
    for (std::size_t game = 0; game <= 3; ++game)
    {
        const Choice c = Choose(monitors, game, 3);
        EXPECT(c.ok);
        EXPECT(c.rowMonitors == 3);
        EXPECT(c.gamesRow == (game != 0));
        EXPECT((c.span == Rect{-1971, 1440, 5709, 2880}));
    }
}

void TestTwoColumnsStartAtTheGame()
{
    std::printf("two columns start at the game's monitor and extend right; shifted left at the row's end\n");
    const auto monitors = RowOfThree();
    Choice     c        = Choose(monitors, 1, 2);
    EXPECT((c.ok && c.span == Rect{-1971, 1440, 3149, 2880}));
    c = Choose(monitors, 2, 2);
    EXPECT((c.ok && c.span == Rect{589, 1440, 5709, 2880}));
    c = Choose(monitors, 3, 2);  // the rightmost: no monitor to its right, so the span shifts left
    EXPECT((c.ok && c.span == Rect{589, 1440, 5709, 2880}));
    c = Choose(monitors, 0, 2);  // the game above the row: from the row's left
    EXPECT((c.ok && c.span == Rect{-1971, 1440, 3149, 2880}));
}

void TestOneColumnIsTheGamesMonitor()
{
    std::printf("one column is the game's own monitor, even an ultrawide alone\n");
    const auto   monitors = RowOfThree();
    const Choice c        = Choose(monitors, 0, 1);
    EXPECT((c.ok && c.span == Rect{0, 0, 3440, 1440} && c.rowMonitors == 1 && c.gamesRow));
}

void TestTheGamesRowWinsATie()
{
    std::printf("two rows of two: the game's own row is taken\n");
    std::vector<Monitor> monitors{At(0, 0), At(2560, 0), At(0, 1440), At(2560, 1440)};
    Choice               c = Choose(monitors, 2, 2);
    EXPECT((c.ok && c.gamesRow && c.span == Rect{0, 1440, 5120, 2880}));
    c = Choose(monitors, 1, 2);
    EXPECT((c.ok && c.gamesRow && c.span == Rect{0, 0, 5120, 1440}));
}

void TestRowBrokenByAnotherSize()
{
    std::printf("the TV at 4K breaks the row: two equal monitors, the reason names the TV\n");
    std::vector<Monitor> monitors{At(0, 0, 3440, 1440), At(-3251, 1440, 3840, 2160), At(589, 1440), At(3149, 1440)};
    const Choice         c = Choose(monitors, 2, 3);
    EXPECT(!c.ok);
    EXPECT(c.rowMonitors == 2);
    EXPECT(c.why.find(L"2 equal monitor(s)") != std::wstring::npos);
    EXPECT(c.why.find(L"3840x2160") != std::wstring::npos);
}

void TestRowBrokenByAMisalignedTop()
{
    std::printf("a monitor 13 px lower is not in the row\n");
    std::vector<Monitor> monitors{At(-1971, 1440), At(589, 1453), At(3149, 1453)};
    const Choice         c = Choose(monitors, 1, 3);
    EXPECT(!c.ok && c.rowMonitors == 2);
    EXPECT(c.why.find(L"top edge") != std::wstring::npos);
}

void TestRowBrokenByAGap()
{
    std::printf("a gap between monitors ends the row; the pair beyond the gap is a row of its own\n");
    std::vector<Monitor> monitors{At(-1971, 1440), At(600, 1440), At(3160, 1440)};
    Choice               c = Choose(monitors, 0, 2);  // the game left of the gap: the pair beyond it is taken
    EXPECT((c.ok && !c.gamesRow && c.rowMonitors == 2 && c.span == Rect{600, 1440, 5720, 2880}));
    std::vector<Monitor> twoApart{At(-1971, 1440), At(600, 1440)};
    c = Choose(twoApart, 0, 2);
    EXPECT(!c.ok && c.rowMonitors == 1);
    EXPECT(c.why.find(L"1 equal monitor(s)") != std::wstring::npos);
}

void TestScalingBreaksTheRow()
{
    std::printf("a scaled neighbour is excluded; a scaled game monitor with no other row refuses outright\n");
    std::vector<Monitor> monitors{At(-1971, 1440, 2560, 1440, 168), At(589, 1440), At(3149, 1440)};
    Choice               c = Choose(monitors, 1, 3);
    EXPECT(!c.ok && c.rowMonitors == 2);
    EXPECT(c.why.find(L"175 %") != std::wstring::npos);
    std::vector<Monitor> scaledAlone{At(0, 0, 3440, 1440, 144)};
    c = Choose(scaledAlone, 0, 1);
    EXPECT(!c.ok && c.why.find(L"the game's monitor is scaled") != std::wstring::npos);
    std::vector<Monitor> scaledGameRowBelow{At(0, 0, 3440, 1440, 144), At(873, 1440), At(3433, 1440)};
    c = Choose(scaledGameRowBelow, 0, 2);  // the game's monitor is scaled, the pair below is not
    EXPECT((c.ok && !c.gamesRow && c.span == Rect{873, 1440, 5993, 2880}));
}

void TestBadInputs()
{
    std::printf("bad inputs refuse\n");
    const auto monitors = RowOfThree();
    EXPECT(!Choose(monitors, 9, 2).ok);
    EXPECT(!Choose(monitors, 1, 0).ok);
    EXPECT(!Choose({}, 0, 1).ok);
}

}  // namespace

int main()
{
    TestRowBelowTheGame();
    TestUsersPairTwoPixelsOff();
    TestThreeAcrossTheRow();
    TestTwoColumnsStartAtTheGame();
    TestOneColumnIsTheGamesMonitor();
    TestTheGamesRowWinsATie();
    TestRowBrokenByAnotherSize();
    TestRowBrokenByAMisalignedTop();
    TestRowBrokenByAGap();
    TestScalingBreaksTheRow();
    TestBadInputs();
    if (g_failures == 0)
    {
        std::printf("all monitor_row tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
