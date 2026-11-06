// Host-side tests for views::monitor_row::Choose, run without the game.
//
// The cases: a desktop with an ultrawide primary above, holding the game (two equal 1440p
// monitors below it: the row is not the game's; the pair 2 px apart, later above the ultrawide 15 px apart: the
// off monitor is listed as a move to the row's top); Rebirth's three-monitor row (the game in it; a two-column span
// starting at the game's monitor and shifted left at the row's end; moves only for the span's columns); the row
// broken by a monitor of another size, a top too far off (beyond the tolerance), a gap, or Windows scaling; the
// game's monitor scaled with and without another row; one column always possible; bad inputs; rows of one height
// and different widths (a 2560 | 3440 | 2560 row), the span's widths listed.

#include "views/monitor_row.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

using lss::views::monitor_row::Arrange;
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

/// Where every monitor of `monitors` ends up after a choice's moves.
std::vector<Rect> After(const std::vector<Monitor>& monitors, const Choice& choice)
{
    std::vector<Rect> rects;
    for (const Monitor& m : monitors)
    {
        Rect rect = m.rect;
        for (const Move& move : choice.moves)
        {
            if (move.from == m.rect)
            {
                rect = {move.toLeft, move.toTop, move.toLeft + rect.Width(), move.toTop + rect.Height()};
            }
        }
        rects.push_back(rect);
    }
    return rects;
}

/// What Windows takes silently or not at all: the primary at its corner, no overlap, one connected desktop.
bool ValidDesktop(const std::vector<Rect>& rects)
{
    const auto overlaps = [](const Rect& a, const Rect& b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; };
    const auto joined   = [](const Rect& a, const Rect& b) {
        return ((a.right == b.left || b.right == a.left) && a.top < b.bottom && b.top < a.bottom) ||
               ((a.bottom == b.top || b.bottom == a.top) && a.left < b.right && b.left < a.right);
    };
    std::vector<std::size_t> reached;
    for (std::size_t i = 0; i < rects.size(); ++i)
    {
        for (std::size_t j = i + 1; j < rects.size(); ++j)
        {
            if (overlaps(rects[i], rects[j]))
            {
                return false;
            }
        }
        if (rects[i].left == 0 && rects[i].top == 0)
        {
            reached.push_back(i);
        }
    }
    if (reached.size() != 1)
    {
        return false;
    }
    for (std::size_t at = 0; at < reached.size(); ++at)
    {
        for (std::size_t i = 0; i < rects.size(); ++i)
        {
            if (std::find(reached.begin(), reached.end(), i) == reached.end() && joined(rects[reached[at]], rects[i]))
            {
                reached.push_back(i);
            }
        }
    }
    return reached.size() == rects.size();
}

/// A row of `picks`, valid and in order, and the same picks again ask for no moves once it stands.
void CheckRow(const std::vector<Monitor>& monitors, const std::vector<std::size_t>& picks, std::size_t mostMoved)
{
    const Choice c = Arrange(monitors, picks);
    EXPECT(c.ok);
    if (!c.ok)
    {
        return;
    }
    const auto rects = After(monitors, c);
    EXPECT(ValidDesktop(rects));
    EXPECT(c.moves.size() <= mostMoved);
    std::int32_t x = c.span.left;
    for (const std::size_t pick : picks)
    {
        EXPECT(rects[pick].left == x && rects[pick].top == c.span.top && rects[pick].bottom <= c.span.bottom);
        x = rects[pick].right;
    }
    EXPECT(x == c.span.right);
    EXPECT(c.widths.size() == picks.size());
    std::vector<Monitor> moved = monitors;
    for (std::size_t i = 0; i < moved.size(); ++i)
    {
        moved[i].rect = rects[i];
    }
    const Choice again = Arrange(moved, picks);
    EXPECT(again.ok && again.moves.empty() && again.span == c.span);
}

/// The player's picks make a row wherever the monitors sit. The desktop: 2560 | 3440 | 2560 with a fourth above.
void PickedRows()
{
    const std::vector<Monitor> desktop{At(0, 0, 3440), At(3440, 0), At(-2560, 0), At(2585, -1440)};
    constexpr std::size_t kPrimary = 0, kRight = 1, kLeft = 2, kAbove = 3;

    // Already a row: nothing moves.
    Choice c = Arrange(desktop, std::vector<std::size_t>{kPrimary, kRight});
    EXPECT((c.ok && c.moves.empty() && c.span == Rect{0, 0, 6000, 1440}));
    c = Arrange(desktop, std::vector<std::size_t>{kLeft, kPrimary, kRight});
    EXPECT((c.ok && c.moves.empty() && c.span == Rect{-2560, 0, 6000, 1440}));
    EXPECT((c.widths == std::vector<std::int32_t>{2560, 3440, 2560}));

    // Anything else moves as little as it can.
    CheckRow(desktop, {kRight, kPrimary}, 2);                // the order turned round: the primary stays at its corner
    CheckRow(desktop, {kLeft, kRight}, 2);                   // two with the primary between them
    CheckRow(desktop, {kLeft, kAbove}, 2);                   // two that touch nothing today
    CheckRow(desktop, {kAbove, kPrimary}, 2);                // the one above beside the primary
    CheckRow(desktop, {kAbove, kPrimary, kLeft}, 3);         // three, one of them displaced
    CheckRow(desktop, {kRight, kPrimary, kLeft}, 3);         // the whole row reversed
    CheckRow(desktop, {kLeft, kRight, kAbove}, 3);           // three without the primary

    // A monitor not picked keeps its place when the row leaves it room and a link to the desktop.
    c = Arrange(desktop, std::vector<std::size_t>{kLeft, kAbove});
    EXPECT(c.ok);
    for (const Move& move : c.moves)
    {
        EXPECT(!(move.from == desktop[kRight].rect));
    }

    // A monitor left floating when its only neighbour leaves goes to a place beside what remains.
    const std::vector<Monitor> stack{At(0, 0, 1920, 1080), At(1920, 0, 1920, 1080), At(1920, -1080, 1920, 1080), At(-1920, 0, 1920, 1080)};
    CheckRow(stack, {3, 1}, 3);

    // A monitor linked only by a corner is not connected: it moves. A desktop that was never valid is made valid.
    CheckRow({At(0, 0, 1920, 1080), At(1920, 0, 1920, 1080), At(0, 1080, 1920, 1080), At(3840, 1080, 1920, 1080)}, {1, 0}, 4);
    CheckRow({At(0, 0), At(-2560, 0), At(100, 1440), At(200, 1440)}, {1, 0}, 2);

    // Rows of different widths; a virtual monitor far out on the desktop.
    CheckRow({At(0, 0, 3440), At(3440, 0), At(6000, 0, 2560)}, {2, 0}, 3);

    // The primary alone among the others: nothing but it is at the corner, so the row goes around it.
    CheckRow({At(0, 0), At(-2560, 0), At(2560, 0), At(0, 1440)}, {3, 2}, 2);

    // What keeps a pick from being a row.
    EXPECT(!Arrange(desktop, std::vector<std::size_t>{kPrimary}).ok);
    EXPECT(!Arrange(desktop, std::vector<std::size_t>{kPrimary, kPrimary}).ok);
    EXPECT(!Arrange(desktop, std::vector<std::size_t>{kPrimary, 9}).ok);
    EXPECT(!Arrange(std::vector<Monitor>{At(10, 0), At(2570, 0)}, std::vector<std::size_t>{0, 1}).ok);  // no primary to keep
}

/// Monitors of different heights and scales: the window is as tall as the tallest, and no other monitor stands in its box, even
/// where it falls off the desktop below a shorter monitor.
void MixedRows()
{
    const std::vector<Monitor> desktop{At(0, 0, 3440), At(3440, 0), At(-2560, 0), At(6000, 0, 1280, 800), At(2585, -1440, 2560, 1440, 144)};
    constexpr std::size_t kPrimary = 0, kRight = 1, kLeft = 2, kDeck = 3, kScaled = 4;
    Choice c = Arrange(desktop, std::vector<std::size_t>{kRight, kDeck});
    EXPECT((c.ok && c.moves.empty() && c.span == Rect{3440, 0, 7280, 1440}));
    EXPECT((c.widths == std::vector<std::int32_t>{2560, 1280} && c.heights == std::vector<std::int32_t>{1440, 800}));

    CheckRow(desktop, {kDeck, kPrimary}, 3);
    CheckRow(desktop, {kDeck, kLeft, kRight}, 4);
    CheckRow(desktop, {kScaled, kDeck}, 3);
    c = Arrange(desktop, std::vector<std::size_t>{kPrimary, kScaled});
    EXPECT(c.ok);

    // A monitor under the shorter one, where the window's box falls off the desktop, has to leave.
    const std::vector<Monitor> under{At(0, 0, 1920, 1080), At(1920, 0, 1280, 800), At(1920, 800, 1280, 800)};
    c = Arrange(under, std::vector<std::size_t>{0, 1});
    EXPECT(c.ok && c.moves.size() == 1 && c.moves[0].from == under[2].rect);
    const auto rects = After(under, c);
    EXPECT(ValidDesktop(rects));
    EXPECT(rects[2].left >= c.span.right || rects[2].top >= c.span.bottom || rects[2].bottom <= c.span.top || rects[2].right <= c.span.left);
    // The primary under a short one's box: the row goes elsewhere.
    CheckRow({At(0, 0, 1920, 1080), At(0, -1080, 1920, 1080), At(1920, -1080, 1280, 800)}, {2, 1}, 2);
}

/// A desktop with the pair aligned: the ultrawide primary above (the game on it), the two
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
    std::printf("the pair as first set up (the right one 2 px lower) is lined up: one move, the span over the row\n");
    std::vector<Monitor> monitors{At(0, 0, 3440, 1440), At(873, 1440), At(3433, 1442)};
    Choice               c = Choose(monitors, 0, 2);
    EXPECT(c.ok && c.rowMonitors == 2 && !c.gamesRow);
    EXPECT((c.span == Rect{873, 1440, 5993, 2880}));
    EXPECT(c.moves.size() == 1);
    EXPECT((c.moves.size() == 1 && c.moves[0] == Move{{3433, 1442, 5993, 2882}, 3433, 1440}));

    // As the game's process sees that desktop: the scaled TV also touches the pair's left monitor.
    std::printf("  with the scaled TV touching the pair's left: the same row and move\n");
    monitors.push_back(At(-2967, 1442, 3840, 2160, 168));
    c = Choose(monitors, 0, 2);
    EXPECT(c.ok && c.rowMonitors == 2 && c.moves.size() == 1);
    EXPECT((c.span == Rect{873, 1440, 5993, 2880}));
}

void TestUsersPairAboveTheUltrawide()
{
    // Seen live: the right monitor sits on the ultrawide, the left one floats 15 px higher. The row's top
    // is the attached monitor's, so the floating one moves down to it (moving the attached one up instead made
    // Windows drop the other by 15 px, and the pair swapped).
    std::printf("the pair above the ultrawide: the left one 15 px higher and floating; it moves down to the right one\n");
    std::vector<Monitor> monitors{At(0, 0, 3440, 1440), At(419, -1455), At(2979, -1440), At(-144, 1440, 3840, 2160, 168)};
    Choice               c = Choose(monitors, 0, 2);
    EXPECT(c.ok && c.rowMonitors == 2 && !c.gamesRow);
    EXPECT((c.span == Rect{419, -1440, 5539, 0}));
    EXPECT((c.moves.size() == 1 && c.moves[0] == Move{{419, -1455, 2979, -15}, 419, -1440}));

    std::printf("  the pair floating as a whole (nothing attached): the leftmost's top, as before\n");
    std::vector<Monitor> floating{At(0, 0, 3440, 1440), At(419, -1470), At(2979, -1455)};
    c = Choose(floating, 0, 2);
    EXPECT((c.ok && c.span == Rect{419, -1470, 5539, -30}));
    EXPECT((c.moves.size() == 1 && c.moves[0] == Move{{2979, -1455, 5539, -15}, 2979, -1470}));

    std::printf("  the pair below the ultrawide with the right one attached and the left one 2 px low: the left moves up\n");
    std::vector<Monitor> below{At(0, 0, 3440, 1440), At(873, 1442), At(3433, 1440)};
    c = Choose(below, 0, 2);
    EXPECT((c.ok && c.span == Rect{873, 1440, 5993, 2880}));
    EXPECT((c.moves.size() == 1 && c.moves[0] == Move{{873, 1442, 3433, 2882}, 873, 1440}));
}

void TestMovesOnlyForTheSpansColumns()
{
    std::printf("a row monitor outside the span's columns is not moved; inside it is\n");
    std::vector<Monitor> monitors{At(-1971, 1440), At(589, 1440), At(3149, 1450)};
    Choice               c = Choose(monitors, 0, 2);  // the game at the left: columns 1 and 2, both level
    EXPECT((c.ok && c.gamesRow && c.span == Rect{-1971, 1440, 3149, 2880} && c.moves.empty()));
    c = Choose(monitors, 1, 2);  // the game in the middle: columns 2 and 3, the third (10 px low) moves
    EXPECT((c.ok && c.gamesRow && c.span == Rect{589, 1440, 5709, 2880}));
    EXPECT((c.moves.size() == 1 && c.moves[0] == Move{{3149, 1450, 5709, 2890}, 3149, 1440}));
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
    EXPECT((c.moves.size() == 2 && c.moves[0] == Move{{589, 1453, 3149, 2893}, 589, 1440} &&
            c.moves[1] == Move{{3149, 1453, 5709, 2893}, 3149, 1440}));
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
    // A 2560 | 3440 | 2560 row at 1440 (the ultrawide primary in the middle, the game
    // on it) and a 2560x1440 above, sitting on the ultrawide.
    std::printf("a 2560 | 3440 | 2560 row: three columns of their own widths; two from the game's monitor\n");
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
    PickedRows();
    MixedRows();
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
