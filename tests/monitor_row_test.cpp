// Host-side tests for views::monitor_row::Choose, run without the game.
//
// The cases: the user's desktop of 2026-10-21 (an ultrawide primary above, holding the game; two equal 1440p
// monitors below it: the row is not the game's; the pair 2 px apart, later above the ultrawide 15 px apart: the
// off monitor is listed as a move to the row's top); Rebirth's three-monitor row (the game in it; a two-column span
// starting at the game's monitor and shifted left at the row's end; moves only for the span's columns); the row
// broken by a monitor of another size, a top too far off (beyond the tolerance), a gap, or Windows scaling; the
// game's monitor scaled with and without another row; one column always possible; bad inputs; rows of one height
// and different widths (the user's 2560 | 3440 | 2560 of 2026-11-02), the span's widths listed.

#include "views/monitor_row.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

using lss::views::monitor_row::Choice;
using lss::views::monitor_row::Choose;
using lss::views::monitor_row::Monitor;
using lss::views::monitor_row::Move;
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

/// The player's pick: only the named monitors join, a row stops at one left out, and a word that cannot be met changes
/// nothing. The user's row of 2560 | 3440 | 2560 with a fourth monitor above it.
void NamedMonitors()
{
    using lss::views::monitor_row::MarkNamed;
    using lss::views::monitor_row::NamesWord;
    const std::vector<Monitor> desktop{At(0, 0, 3440), At(3440, 0), At(-2560, 0), At(2585, -1440)};
    const std::vector<std::wstring> ids{L"middle", L"right", L"left", L"above"};

    auto list = desktop;
    auto named = MarkNamed(list, ids, std::wstring(L"middle|right"));
    EXPECT(named.named == 2 && named.present == 2);
    EXPECT(list[0].joins && list[1].joins && !list[2].joins && !list[3].joins);
    Choice c = Choose(list, 0, 2);
    EXPECT((c.ok && c.span == Rect{0, 0, 6000, 1440} && c.rowMonitors == 2));
    // Three columns asked of two named monitors: the count the mod shrinks by.
    c = Choose(list, 0, 3);
    EXPECT(!c.ok && c.rowMonitors == 2);
    // The game on a monitor left out: the row is still the named one, starting at its left.
    c = Choose(list, 2, 2);
    EXPECT((c.ok && !c.gamesRow && c.span == Rect{0, 0, 6000, 1440}));

    // Named monitors with one left out between them are no row.
    list  = desktop;
    named = MarkNamed(list, ids, std::wstring(L"left|right"));
    EXPECT(named.present == 2 && !list[0].joins);
    c = Choose(list, 2, 2);
    EXPECT(!c.ok && c.rowMonitors == 1);

    // One named monitor connected, or none named: every monitor still joins.
    list  = desktop;
    named = MarkNamed(list, ids, std::wstring(L"middle|gone"));
    EXPECT(named.named == 2 && named.present == 1 && list[1].joins && list[2].joins);
    list  = desktop;
    named = MarkNamed(list, ids, std::wstring_view{});
    EXPECT(named.named == 0 && named.present == 0 && list[3].joins);

    EXPECT(NamesWord(std::vector<std::wstring>{L"a", L"b"}) == L"a|b");
    EXPECT(NamesWord(std::vector<std::wstring>{}).empty());
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
    EXPECT(c.moves.empty());
}

void TestUsersPairTwoPixelsOff()
{
    std::printf("the user's pair as first set up (the right one 2 px lower) is lined up: one move, the span over the row\n");
    std::vector<Monitor> monitors{At(0, 0, 3440, 1440), At(873, 1440), At(3433, 1442)};
    Choice               c = Choose(monitors, 0, 2);
    EXPECT(c.ok && c.rowMonitors == 2 && !c.gamesRow);
    EXPECT((c.span == Rect{873, 1440, 5993, 2880}));
    EXPECT(c.moves.size() == 1);
    EXPECT((c.moves.size() == 1 && c.moves[0] == Move{{3433, 1442, 5993, 2882}, 1440}));

    // As the game's process sees that desktop (2026-10-21 22:23): the scaled TV also touches the pair's left monitor.
    std::printf("  with the scaled TV touching the pair's left: the same row and move\n");
    monitors.push_back(At(-2967, 1442, 3840, 2160, 168));
    c = Choose(monitors, 0, 2);
    EXPECT(c.ok && c.rowMonitors == 2 && c.moves.size() == 1);
    EXPECT((c.span == Rect{873, 1440, 5993, 2880}));
}

void TestUsersPairAboveTheUltrawide()
{
    // Seen live 2026-10-22: the right monitor sits on the ultrawide, the left one floats 15 px higher. The row's top
    // is the attached monitor's, so the floating one moves down to it (moving the attached one up instead made
    // Windows drop the other by 15 px, and the pair swapped).
    std::printf("the user's pair above the ultrawide (22:41): the left one 15 px higher and floating; it moves down to the right one\n");
    std::vector<Monitor> monitors{At(0, 0, 3440, 1440), At(419, -1455), At(2979, -1440), At(-144, 1440, 3840, 2160, 168)};
    Choice               c = Choose(monitors, 0, 2);
    EXPECT(c.ok && c.rowMonitors == 2 && !c.gamesRow);
    EXPECT((c.span == Rect{419, -1440, 5539, 0}));
    EXPECT((c.moves.size() == 1 && c.moves[0] == Move{{419, -1455, 2979, -15}, -1440}));

    std::printf("  the pair floating as a whole (nothing attached): the leftmost's top, as before\n");
    std::vector<Monitor> floating{At(0, 0, 3440, 1440), At(419, -1470), At(2979, -1455)};
    c = Choose(floating, 0, 2);
    EXPECT((c.ok && c.span == Rect{419, -1470, 5539, -30}));
    EXPECT((c.moves.size() == 1 && c.moves[0] == Move{{2979, -1455, 5539, -15}, -1470}));

    std::printf("  the pair below the ultrawide with the right one attached and the left one 2 px low: the left moves up\n");
    std::vector<Monitor> below{At(0, 0, 3440, 1440), At(873, 1442), At(3433, 1440)};
    c = Choose(below, 0, 2);
    EXPECT((c.ok && c.span == Rect{873, 1440, 5993, 2880}));
    EXPECT((c.moves.size() == 1 && c.moves[0] == Move{{873, 1442, 3433, 2882}, 1440}));
}

void TestMovesOnlyForTheSpansColumns()
{
    std::printf("a row monitor outside the span's columns is not moved; inside it is\n");
    std::vector<Monitor> monitors{At(-1971, 1440), At(589, 1440), At(3149, 1450)};
    Choice               c = Choose(monitors, 0, 2);  // the game at the left: columns 1 and 2, both level
    EXPECT((c.ok && c.gamesRow && c.span == Rect{-1971, 1440, 3149, 2880} && c.moves.empty()));
    c = Choose(monitors, 1, 2);  // the game in the middle: columns 2 and 3, the third (10 px low) moves
    EXPECT((c.ok && c.gamesRow && c.span == Rect{589, 1440, 5709, 2880}));
    EXPECT((c.moves.size() == 1 && c.moves[0] == Move{{3149, 1450, 5709, 2890}, 1440}));
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
    std::printf("the TV at 4K breaks the row (another height): two monitors, the reason names the TV\n");
    std::vector<Monitor> monitors{At(0, 0, 3440, 1440), At(-3251, 1440, 3840, 2160), At(589, 1440), At(3149, 1440)};
    const Choice         c = Choose(monitors, 2, 3);
    EXPECT(!c.ok);
    EXPECT(c.rowMonitors == 2);
    EXPECT(c.why.find(L"2 monitor(s) of one height") != std::wstring::npos);
    EXPECT(c.why.find(L"3840x2160") != std::wstring::npos);
}

void TestRowBrokenByAMisalignedTop()
{
    std::printf("two monitors 13 px lower are lined up to the leftmost's top; 40 px lower is refused, naming the move\n");
    std::vector<Monitor> monitors{At(-1971, 1440), At(589, 1453), At(3149, 1453)};
    Choice               c = Choose(monitors, 1, 3);
    EXPECT((c.ok && c.gamesRow && c.rowMonitors == 3 && c.span == Rect{-1971, 1440, 5709, 2880}));
    EXPECT(c.moves.size() == 2);
    EXPECT((c.moves.size() == 2 && c.moves[0] == Move{{589, 1453, 3149, 2893}, 1440} &&
            c.moves[1] == Move{{3149, 1453, 5709, 2893}, 1440}));
    std::vector<Monitor> far{At(-1971, 1440), At(589, 1480), At(3149, 1480)};
    c = Choose(far, 1, 3);
    EXPECT(!c.ok && c.rowMonitors == 2);
    EXPECT(c.why.find(L"the monitor at (589, 1480) sits 40 px lower than its neighbour at (-1971, 1440)") !=
           std::wstring::npos);
    EXPECT(c.why.find(L"more than the 32 px") != std::wstring::npos);
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
    EXPECT(c.why.find(L"1 monitor(s) of one height") != std::wstring::npos);
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

void TestOneHeightOfDifferentWidths()
{
    // The user's desktop of 2026-11-02: a 2560 | 3440 | 2560 row at 1440 (the ultrawide primary in the middle, the game
    // on it) and a 2560x1440 above, sitting on the ultrawide.
    std::printf("the user's 2560 | 3440 | 2560 row: three columns of their own widths; two from the game's monitor\n");
    std::vector<Monitor> monitors{At(3440, 0), At(0, 0, 3440, 1440), At(-2560, 0), At(2546, -1440)};
    Choice               c = Choose(monitors, 1, 3);
    EXPECT((c.ok && c.gamesRow && c.rowMonitors == 3 && c.span == Rect{-2560, 0, 6000, 1440} && c.moves.empty()));
    EXPECT((c.widths == std::vector<std::int32_t>{2560, 3440, 2560}));
    c = Choose(monitors, 1, 2);
    EXPECT((c.ok && c.span == Rect{0, 0, 6000, 1440} && (c.widths == std::vector<std::int32_t>{3440, 2560})));
    c = Choose(monitors, 0, 2);  // the game on the right one: shifted left at the row's end
    EXPECT((c.ok && c.span == Rect{0, 0, 6000, 1440}));
    c = Choose(monitors, 3, 3);  // the game on the monitor above: the row below it
    EXPECT((c.ok && !c.gamesRow && c.span == Rect{-2560, 0, 6000, 1440}));
    c = Choose(monitors, 1, 1);
    EXPECT((c.ok && (c.widths == std::vector<std::int32_t>{3440})));

    std::printf("  another height in the row still ends it, naming the height\n");
    std::vector<Monitor> mixed{At(0, 0, 3440, 1440), At(3440, 0, 1920, 1080), At(-2560, 0)};
    c = Choose(mixed, 0, 3);
    EXPECT(!c.ok && c.rowMonitors == 2);
    EXPECT(c.why.find(L"1080 px tall where its neighbour is 1440") != std::wstring::npos);
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
    NamedMonitors();
    TestRowBelowTheGame();
    TestUsersPairTwoPixelsOff();
    TestUsersPairAboveTheUltrawide();
    TestMovesOnlyForTheSpansColumns();
    TestThreeAcrossTheRow();
    TestTwoColumnsStartAtTheGame();
    TestOneColumnIsTheGamesMonitor();
    TestTheGamesRowWinsATie();
    TestRowBrokenByAnotherSize();
    TestRowBrokenByAMisalignedTop();
    TestRowBrokenByAGap();
    TestScalingBreaksTheRow();
    TestOneHeightOfDifferentWidths();
    TestBadInputs();
    if (g_failures == 0)
    {
        std::printf("all monitor_row tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
