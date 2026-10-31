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
void LayoutRecords()
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

/// What the module does with the entry it read, given the entry wanted.
void Decisions()
{
    EXPECT(Decide(kStockFavorTop, kThreeColumns) == Action::Write);
    EXPECT(Decide(kStockFavorTop, kStockFavorTop) == Action::Nothing);
    EXPECT(Decide(kThreeColumns, kThreeColumns) == Action::Nothing);
    EXPECT(Decide(kThreeColumns, kStockFavorTop) == Action::Restore);
    Record foreign[kThreePlayerRecords] = {{0.5f, 1.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.0f}};
    EXPECT(Decide(foreign, kThreeColumns) == Action::Foreign);
    EXPECT(Decide(foreign, kStockFavorTop) == Action::Foreign);
    Record zero[kThreePlayerRecords] = {};
    EXPECT(Decide(zero, kThreeColumns) == Action::Foreign);
    // Partly the mod's (two records written, the third stock): foreign, never "restored" over.
    Record partly[kThreePlayerRecords] = {kThreeColumns[0], kThreeColumns[1], kStockFavorTop[2]};
    EXPECT(Decide(partly, kStockFavorTop) == Action::Foreign);
    EXPECT(Decide(partly, kThreeColumns) == Action::Foreign);
    // A record twice is no arrangement: foreign.
    Record twice[kThreePlayerRecords] = {kThreeColumns[0], kThreeColumns[1], kThreeColumns[1]};
    EXPECT(Decide(twice, kThreeColumns) == Action::Foreign);
    // The FavorBottom entry's records in the FavorTop slot: foreign.
    Record favorBottom[kThreePlayerRecords] = {{0.5f, 0.5f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.0f}, {1.0f, 0.5f, 0.0f, 0.5f}};
    EXPECT(Decide(favorBottom, kThreeColumns) == Action::Foreign);
    // A NaN never equals anything: foreign, left alone.
    Record nan[kThreePlayerRecords] = {kStockFavorTop[0], kStockFavorTop[1], {0.5f, 0.5f, 0.5f, std::numeric_limits<float>::quiet_NaN()}};
    EXPECT(Decide(nan, kThreeColumns) == Action::Foreign);
    // Negative zero equals zero: still the stock entry.
    Record negativeZero[kThreePlayerRecords] = {{1.0f, 0.5f, -0.0f, -0.0f}, kStockFavorTop[1], kStockFavorTop[2]};
    EXPECT(Decide(negativeZero, kThreeColumns) == Action::Write);
    // An arranged entry of the mod's: written over by another arrangement, restored to the stock one.
    Record swapped[kThreePlayerRecords] = {kThreeColumns[0], kThreeColumns[2], kThreeColumns[1]};
    EXPECT(Decide(swapped, kThreeColumns) == Action::Write);
    EXPECT(Decide(swapped, kStockFavorTop) == Action::Restore);
    EXPECT(Decide(swapped, swapped) == Action::Nothing);
    Record swappedTop[kThreePlayerRecords] = {kStockFavorTop[0], kStockFavorTop[2], kStockFavorTop[1]};
    EXPECT(Decide(swappedTop, kStockFavorTop) == Action::Restore);
    EXPECT(Decide(kStockFavorTop, swappedTop) == Action::Write);
}

/// The column order from the settings file or the console: the three players, each once, in any order.
void Orders()
{
    EXPECT(ParseOrder(L"1 2 3") == kJoinOrder);
    EXPECT(ParseOrder(L"123") == kJoinOrder);
    EXPECT((ParseOrder(L"1, 3, 2") == Order{{1, 3, 2}}));
    EXPECT((ParseOrder(L" 132 ") == Order{{1, 3, 2}}));
    EXPECT((ParseOrder(L"213") == Order{{2, 1, 3}}));  // player 1 may take any screen
    EXPECT((ParseOrder(L"3 2 1") == Order{{3, 2, 1}}));
    EXPECT(!ParseOrder(L"124"));
    EXPECT(!ParseOrder(L"012"));
    EXPECT(!ParseOrder(L"12"));
    EXPECT(!ParseOrder(L"1233"));
    EXPECT(!ParseOrder(L"122"));
    EXPECT(!ParseOrder(L""));
    EXPECT(!ParseOrder(L"one two"));
}

/// Each view's slot: a player's from the order, the mirrors' the ones left over in list order.
void SlotsAndEntries()
{
    using Slots3 = std::array<int, kThreePlayerRecords>;
    const Order swap{{1, 3, 2}};
    EXPECT((Slots(kJoinOrder, {0, 1, 2}) == Slots3{0, 1, 2}));
    EXPECT((Slots(swap, {0, 1, 2}) == Slots3{0, 2, 1}));   // player 2 joined first: right; player 3 middle
    EXPECT((Slots(swap, {0, 2, 1}) == Slots3{0, 1, 2}));   // player 3 joined first: already in the middle
    EXPECT((Slots(swap, {0, 1, -1}) == Slots3{0, 2, 1}));  // player 2 and the mirror: the mirror in the middle
    EXPECT((Slots(kJoinOrder, {0, 2, -1}) == Slots3{0, 2, 1}));  // Slots alone (EntryWanted does not arrange under the join order)
    EXPECT((Slots(kJoinOrder, {0, -1, -1}) == Slots3{0, 1, 2}));  // two mirrors in list order
    EXPECT((Slots(swap, {0, -1, -1}) == Slots3{0, 1, 2}));
    // Three views on a span: the columns arranged; the join order and the stock entry: nothing to write in one window.
    const Records span = EntryWanted(true, true, swap, {0, 1, 2});
    EXPECT(span.record[0] == kThreeColumns[0] && span.record[1] == kThreeColumns[2] && span.record[2] == kThreeColumns[1]);
    EXPECT(Same(EntryWanted(false, true, kJoinOrder, {0, 1, 2}).record, kStockFavorTop));
    const Records window = EntryWanted(false, true, swap, {0, 1, 2});
    EXPECT(window.record[1] == kStockFavorTop[2] && window.record[2] == kStockFavorTop[1]);
    EXPECT(Same(EntryWanted(false, false, swap, {0, 1, -1}).record, kStockFavorTop));  // two views: the entry is unused
    EXPECT(Same(EntryWanted(true, true, kJoinOrder, {0, 1, 2}).record, kThreeColumns));
    // The join order arranges nothing: whoever joined first keeps the middle (bottom left), as before the setting.
    EXPECT(Same(EntryWanted(true, true, kJoinOrder, {0, 2, -1}).record, kThreeColumns));
    EXPECT(Same(EntryWanted(true, true, kJoinOrder, {0, 2, 1}).record, kThreeColumns));
    EXPECT(Same(EntryWanted(false, true, kJoinOrder, {0, 2, 1}).record, kStockFavorTop));
    // 1 3 2 with player 3 alone beside a mirror: player 3 keeps the middle, the mirror the right.
    EXPECT(Same(EntryWanted(true, true, swap, {0, 2, -1}).record, kThreeColumns));
}

}  // namespace

/// The pad claim's orders (players/pad_claim): by player number whatever the join order, a slot no player has taking the
/// player left over; player 1 on any slot.
void ClaimOrders()
{
    using Seats3 = std::array<int, kThreePlayerRecords>;
    const auto both = OrderShowing(Seats3{0, 1, 2});
    EXPECT(both && both->byNumber && (both->player == Seats3{1, 2, 3}));
    EXPECT(!(*both == kJoinOrder));  // arranged, unlike the join order's 1 2 3
    const auto swapped = OrderShowing(Seats3{0, 2, 1});
    EXPECT(swapped && (swapped->player == Seats3{1, 3, 2}));
    const auto mirror = OrderShowing(Seats3{0, 1, -1});  // player 2 and a mirror
    EXPECT(mirror && (mirror->player == Seats3{1, 2, 3}));
    const auto moved = OrderShowing(Seats3{1, 0, 2});  // player 1 in the middle
    EXPECT(moved && (moved->player == Seats3{2, 1, 3}));
    const auto twoMoved = OrderShowing(Seats3{1, 0, -1});  // two screens: player 2 first, player 1 second
    EXPECT(twoMoved && (twoMoved->player == Seats3{2, 1, 3}));
    EXPECT(!OrderShowing(Seats3{0, 1, 1}));   // a seat twice
    EXPECT(!OrderShowing(Seats3{0, 3, 1}));   // no such seat

    // Player 3 joined first (the list: player 1, player 3, player 2): by number 1 2 3 puts player 2 in the middle.
    using Slots3 = std::array<int, kThreePlayerRecords>;
    EXPECT((ScreenSlots(3, *both, {0, 2, 1}) == Slots3{0, 2, 1}));
    EXPECT((ScreenSlots(3, kJoinOrder, {0, 2, 1}) == Slots3{0, 1, 2}));  // the join order arranges nothing
    EXPECT((ScreenSlots(2, *both, {0, 2, 1}) == Slots3{0, 1, 2}));      // two views, player 1 before player 3: in place
    EXPECT((ScreenSlots(2, *twoMoved, {0, 1, -1}) == Slots3{1, 0, 2}));  // two views, player 2 first: swapped
    EXPECT((ScreenSlots(1, *twoMoved, {0, 1, -1}) == Slots3{0, 1, 2}));  // one view: nothing to arrange
}

/// Two views: the engine's two-player entries, the two players in the order's order (the one not in skipped).
void TwoViews()
{
    using Slots2 = std::array<int, kTwoPlayerRecords>;
    const Order p2First{{2, 1, 3}};
    const Order p3First{{3, 2, 1}};
    EXPECT((TwoSlots(kJoinOrder, {0, 1, -1}) == Slots2{0, 1}));
    EXPECT((TwoSlots(p2First, {0, 1, -1}) == Slots2{1, 0}));   // player 1 on the second screen
    EXPECT((TwoSlots(p2First, {0, 2, -1}) == Slots2{0, 1}));   // player 3 in: 2 1 3 puts player 1 first of the two
    EXPECT((TwoSlots(p3First, {0, 2, -1}) == Slots2{1, 0}));
    EXPECT((TwoSlots(p2First, {0, -1, -1}) == Slots2{0, 1}));  // a mirror: the list's order
    EXPECT((TwoSlots(Order{{1, 3, 2}}, {0, 1, -1}) == Slots2{0, 1}));
    // The entries: stock under the join order or with three views, the records swapped for player 1 second.
    EXPECT(Same(TwoEntryWanted(kStockTopBottom, true, kJoinOrder, {0, 1, -1}).record, kStockTopBottom));
    EXPECT(Same(TwoEntryWanted(kStockSideBySide, false, p2First, {0, 1, 2}).record, kStockSideBySide));
    const TwoRecords swapped = TwoEntryWanted(kStockSideBySide, true, p2First, {0, 1, -1});
    // RE-layout-fov.md §9.3's swapped values: player 1 on the right half, player 2 on the left.
    EXPECT((swapped.record[0] == Record{0.5f, 1.0f, 0.5f, 0.0f}) && (swapped.record[1] == Record{0.5f, 1.0f, 0.0f, 0.0f}));
    const TwoRecords below = TwoEntryWanted(kStockTopBottom, true, p2First, {0, 1, -1});
    EXPECT((below.record[0] == Record{1.0f, 0.5f, 0.0f, 0.5f}) && (below.record[1] == Record{1.0f, 0.5f, 0.0f, 0.0f}));
    // Decisions: the stock entry written over, the mod's restored, anything else left alone.
    EXPECT(DecideTwo(kStockSideBySide, swapped.record, kStockSideBySide) == Action::Write);
    EXPECT(DecideTwo(swapped.record, kStockSideBySide, kStockSideBySide) == Action::Restore);
    EXPECT(DecideTwo(swapped.record, swapped.record, kStockSideBySide) == Action::Nothing);
    EXPECT(DecideTwo(kStockSideBySide, kStockSideBySide, kStockSideBySide) == Action::Nothing);
    EXPECT(DecideTwo(kStockTopBottom, swapped.record, kStockSideBySide) == Action::Foreign);
}

int main()
{
    WantedColumns();
    ColumnsOrStock();
    LayoutRecords();
    Decisions();
    Orders();
    SlotsAndEntries();
    ClaimOrders();
    TwoViews();
    std::printf(g_failures == 0 ? "column_rules: all passed\n" : "column_rules: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
