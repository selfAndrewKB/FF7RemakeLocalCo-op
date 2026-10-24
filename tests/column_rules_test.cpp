// Host-side tests for the view columns' pure rules (views/column_rules.h). Run without the game.

#include "views/column_rules.h"

#include <cstdio>
#include <limits>

namespace {

using namespace lss::views::column_rules;

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

/// The columns a frame shows: the players, the span's columns, the console's override, within one and the most.
void WantedColumns()
{
    EXPECT(Wanted(1, 0, 0) == 1);  // player 1 alone, no span
    EXPECT(Wanted(2, 0, 0) == 2);  // player 2 in, no span: the split
    EXPECT(Wanted(1, 2, 0) == 2);  // the span held over two monitors with player 2 out: the mirror's column
    EXPECT(Wanted(2, 2, 0) == 2);
    EXPECT(Wanted(1, 3, 0) == 3);  // a three-monitor row: two mirrors
    EXPECT(Wanted(2, 3, 0) == 3);  // one mirror beside the two players
    EXPECT(Wanted(3, 3, 0) == 3);
    EXPECT(Wanted(1, 0, 3) == 3);  // the console's override on one monitor
    EXPECT(Wanted(2, 2, 3) == 3);  // the override over a two-monitor span: three columns across the pair
    EXPECT(Wanted(2, 3, 2) == 2);  // the override may ask for fewer than the span, never fewer than the players
    EXPECT(Wanted(3, 0, 1) == 3);
    EXPECT(Wanted(1, 7, 0) == kMaxColumns);  // never beyond the columns built
    EXPECT(Wanted(4, 0, 0) == kMaxColumns);  // more players than columns built: the most wins
    EXPECT(Wanted(0, 0, 0) == 1);            // bad input: at least one
    EXPECT(Wanted(-1, -2, -3) == 1);
    EXPECT(Wanted(2, 2, -1) == 2);           // a negative override counts as none
    static_assert(Wanted(2, 3, 0) == 3, "constexpr");
}

/// Columns on a span or by the console's override; in one window the engine's top-and-bottom entry.
void ColumnsOrStock()
{
    EXPECT(ThreeColumns(3, 3, 3, 0));    // three players over three monitors
    EXPECT(ThreeColumns(3, 3, 2, 0));    // spanned over two: still columns (no layout keeps every view off that seam)
    EXPECT(ThreeColumns(3, 2, 3, 0));    // two players and a mirror over three monitors
    EXPECT(!ThreeColumns(3, 3, 0, 0));   // three players in one window: player 1 on top, two below
    EXPECT(ThreeColumns(3, 3, 0, 3));    // the console's override in one window
    EXPECT(!ThreeColumns(2, 2, 2, 0));   // two views: the entry is not used
    EXPECT(ThreeColumns(2, 3, 2, 0));    // a mirror the engine still lists for a tick: kept in its column
    EXPECT(!ThreeColumns(2, 2, 0, 3));   // an override with two views: nothing to lay out
    static_assert(!ThreeColumns(3, 3, 0, 0), "constexpr");
}

/// The layout records: the stock entry and the mod's columns are distinct, the columns tile the width exactly.
void Records()
{
    EXPECT(!Same(kStockFavorTop, kThreeColumns));
    EXPECT(Same(kThreeColumns, kThreeColumns));
    float width = 0;
    for (int i = 0; i < kThreePlayerRecords; ++i)
    {
        EXPECT(kThreeColumns[i].sizeY == 1.0f && kThreeColumns[i].originY == 0.0f);
        EXPECT(kThreeColumns[i].originX == width);
        width += kThreeColumns[i].sizeX;
    }
    EXPECT(width > 0.999f && width < 1.001f);
    EXPECT(kStockFavorTop[0].sizeX == 1.0f && kStockFavorTop[2].originX == 0.5f && kStockFavorTop[2].originY == 0.5f);
}

/// What the module does with the entry it read.
void Decisions()
{
    EXPECT(Decide(kStockFavorTop, true) == Action::Write);
    EXPECT(Decide(kStockFavorTop, false) == Action::Nothing);
    EXPECT(Decide(kThreeColumns, true) == Action::Nothing);
    EXPECT(Decide(kThreeColumns, false) == Action::Restore);
    Record foreign[kThreePlayerRecords] = {{0.5f, 1.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.0f}};
    EXPECT(Decide(foreign, true) == Action::Foreign);
    EXPECT(Decide(foreign, false) == Action::Foreign);
    Record zero[kThreePlayerRecords] = {};
    EXPECT(Decide(zero, true) == Action::Foreign);
    // Partly the mod's (two records written, the third stock): foreign, never "restored" over.
    Record partly[kThreePlayerRecords] = {kThreeColumns[0], kThreeColumns[1], kStockFavorTop[2]};
    EXPECT(Decide(partly, false) == Action::Foreign);
    EXPECT(Decide(partly, true) == Action::Foreign);
    // The FavorBottom entry's records in the FavorTop slot: foreign.
    Record favorBottom[kThreePlayerRecords] = {{0.5f, 0.5f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.0f}, {1.0f, 0.5f, 0.0f, 0.5f}};
    EXPECT(Decide(favorBottom, true) == Action::Foreign);
    // A NaN never equals anything: foreign, left alone.
    Record nan[kThreePlayerRecords] = {kStockFavorTop[0], kStockFavorTop[1], {0.5f, 0.5f, 0.5f, std::numeric_limits<float>::quiet_NaN()}};
    EXPECT(Decide(nan, true) == Action::Foreign);
    // Negative zero equals zero: still the stock entry.
    Record negativeZero[kThreePlayerRecords] = {{1.0f, 0.5f, -0.0f, -0.0f}, kStockFavorTop[1], kStockFavorTop[2]};
    EXPECT(Decide(negativeZero, true) == Action::Write);
}

}  // namespace

int main()
{
    WantedColumns();
    ColumnsOrStock();
    Records();
    Decisions();
    std::printf(g_failures == 0 ? "column_rules: all passed\n" : "column_rules: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
