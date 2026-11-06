// Host-side tests for the view columns' map (views/columns.h): the engine's list as columns, each column's seat or
// mirror and each seat's column. Run without the game. The rows of hud/overlay_pass's old seat table are here.

#include "views/columns.h"

#include <array>
#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "columns: all passed\n" : "columns: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
}  // namespace lss::test

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++lss::test::g_failures;                                         \
        }                                                                    \
    } while (false)

namespace {

using namespace lss::views::columns;

constexpr std::array<int, 1> kOnePlayer{0};
constexpr std::array<int, 2> kTwoPlayers{0, 1};
constexpr std::array<int, 3> kThreePlayers{0, 1, 2};
constexpr std::array<int, 3> kTwoAndMirror{0, 1, kMirror};
constexpr std::array<int, 3> kOneAndMirrors{0, kMirror, kMirror};
constexpr std::array<int, 3> kThreeOutOfOrder{0, 2, kMirror};  // player 3 in with player 2 out, a mirror third
constexpr std::array<int, 3> kSwapped{0, 2, 1};                // player 3 joined first, player 2 after

void Players()
{
    const Map one = MapOf(kOnePlayer);
    EXPECT(one.count == 1 && one.SeatOf(0) == 0 && one.SeatOf(1) == kUnknown && one.SeatOf(2) == kUnknown);
    EXPECT(one.ColumnOf(0) == 0 && one.ColumnOf(1) == -1 && one.ColumnOf(2) == -1 && one.mirrors == 0);
    const Map two = MapOf(kTwoPlayers);
    EXPECT(two.count == 2 && two.ColumnOf(1) == 1 && two.SeatOf(1) == 1);
    const Map three = MapOf(kThreePlayers);
    EXPECT(three.count == 3 && three.ColumnOf(1) == 1 && three.ColumnOf(2) == 2 && three.SeatOf(2) == 2);
    const Map swapped = MapOf(kSwapped);
    EXPECT(swapped.ColumnOf(1) == 2 && swapped.ColumnOf(2) == 1 && swapped.SeatOf(1) == 2);
    static_assert(MapOf(kTwoPlayers).count == 2, "constexpr");
}

void Mirrors()
{
    const Map m = MapOf(kTwoAndMirror);
    EXPECT(m.count == 3 && m.mirrors == 1 && m.SeatOf(2) == kMirror);
    const Map mm = MapOf(kOneAndMirrors);
    EXPECT(mm.count == 3 && mm.mirrors == 2 && mm.SeatOf(1) == kMirror && mm.SeatOf(2) == kMirror);
    const Map out = MapOf(kThreeOutOfOrder);
    EXPECT(out.ColumnOf(2) == 1 && out.ColumnOf(1) == -1 && out.SeatOf(2) == kMirror && out.mirrors == 1);
}

void Edges()
{
    // An unknown place (a local player the mod does not know, an unread one) is no one's and no mirror's.
    constexpr std::array<int, 2> unknown{0, kUnknown};
    const Map u = MapOf(unknown);
    EXPECT(u.count == 2 && u.SeatOf(1) == kUnknown && u.mirrors == 0);
    // A seat listed twice keeps its first column; the later place reads unknown.
    constexpr std::array<int, 3> twice{0, 1, 1};
    const Map t = MapOf(twice);
    EXPECT(t.ColumnOf(1) == 1 && t.SeatOf(2) == kUnknown);
    // More places than columns: the first kMaxColumns count.
    constexpr std::array<int, 5> many{0, 1, 2, kMirror, kMirror};
    const Map m = MapOf(many);
    EXPECT(m.count == kMaxColumns && m.mirrors == 0);
    // An empty list.
    const Map e = MapOf(std::array<int, 0>{});
    EXPECT(e.count == 0 && e.SeatOf(0) == kUnknown);
    // Out of range.
    EXPECT(m.SeatOf(-1) == kUnknown && m.SeatOf(kMaxColumns) == kUnknown && m.ColumnOf(-1) == -1 && m.ColumnOf(99) == -1);
}

}  // namespace

int main()
{
    Players();
    Mirrors();
    Edges();
    return lss::test::Report();
}
