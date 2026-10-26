// Host-side tests for views/column_source.h: where each column's view comes from, per moment and per surface (the
// table in plan/next-phase.md §7). Surfaces: one window with two or three players, the span (columns held) with
// and without a mirror. Moments: field play, a scene, a seat's own shop, a handed seat, the console's override.
// Rebirth's tests/column_source_test.cpp gave the shape and the rows that apply.

#include "views/column_source.h"

#include <array>
#include <cstdio>
#include <string>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "column_source: all passed\n" : "column_source: %d failure(s)\n", g_failures);
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

using namespace lss::views::column_source;
using lss::players::SeatState;
using lss::views::columns::kMirror;
using lss::views::columns::MapOf;

// Named: a brace literal splits the EXPECT macro.
constexpr SeatSource kSelf{Source::Self, Why::None};
constexpr SeatSource kColumnConsole{Source::PlayerOne, Why::Console};
constexpr SeatSource kScreenConsole{Source::Screen, Why::Console};

/// A moment: the places of the engine's list (a seat, kMirror), each seat's word, the rest by name.
template <std::size_t N>
constexpr Moment At(const std::array<int, N>& places, SeatState seat1, SeatState seat2, bool held = false,
                    Override choice = Override::Auto, bool scene = false, bool shop1 = false, bool screenBuilt = false)
{
    Moment m;
    m.map         = MapOf(places);
    m.seats[1]    = seat1;
    m.seats[2]    = seat2;
    m.columnsHeld = held;
    m.choice      = choice;
    m.sceneHolds  = scene;
    m.ownShop[1]  = shop1;
    m.screenBuilt = screenBuilt;
    return m;
}

constexpr std::array<int, 1> kOnePlayer{0};
constexpr std::array<int, 2> kTwoPlayers{0, 1};
constexpr std::array<int, 3> kThreePlayers{0, 1, 2};
constexpr std::array<int, 3> kTwoAndMirror{0, 1, kMirror};
constexpr std::array<int, 3> kOneAndMirrors{0, kMirror, kMirror};
constexpr std::array<int, 3> kThreeOutOfOrder{0, 2, kMirror};  // player 3 in, player 2 out

constexpr bool kHeld = true;

/// Field play: every column its player's own; a scene and a seat's own shop are Self's own mechanisms.
void FieldPlay()
{
    {
        const Shown s = Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out));
        EXPECT(!s.Single() && s.SplitShown() && s.ViewsSplit() && s.columnCount == 2);
        EXPECT(s.seats[1] == kSelf && s.columns[1] == kSelf && !s.OnPlayerOne(1));
        EXPECT(s.ListenerColumn() == 1);
    }
    {
        const Shown s = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Playing));
        EXPECT(!s.Single() && s.SplitShown() && s.columnCount == 3 && s.columns[1] == kSelf && s.columns[2] == kSelf);
    }
    {
        // A scene holds the players: every seat keeps its own view (the scene camera plays on it, v0.40.0). The scene
        // is a fact of the moment that no rule reads today; the row pins the answer for when a moment reads it.
        const Shown s = Decide(At(kThreePlayers, SeatState::Handed, SeatState::Playing, false, Override::Auto, true));
        EXPECT(s.seats[1] == kSelf && s.seats[2] == kSelf && s.SplitShown() && !s.OnPlayerOne(1));
    }
    {
        // A seat's own shop: its view is its own (the shop camera on it), even under `single-view on`; the other
        // seat's column goes to player 1's picture.
        const Shown s = Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out, false, Override::Auto, true, true));
        EXPECT(s.seats[1] == kSelf && !s.OnPlayerOne(1));
        const Shown on = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Playing, kHeld, Override::Single, false, true));
        EXPECT(on.seats[1] == kSelf && !on.OnPlayerOne(1) && on.OnPlayerOne(2) && on.SplitShown());
    }
    {
        // A handed seat keeps its own view (its camera on the handed member, or the scene camera).
        const Shown s = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Out));
        EXPECT(s.seats[1] == kSelf && s.SplitShown() && !s.Single());
    }
    {
        // Player 1 alone: nothing to decide, one column.
        const Shown s = Decide(At(kOnePlayer, SeatState::Out, SeatState::Out));
        EXPECT(!s.Single() && !s.SplitShown() && !s.ViewsSplit() && s.columnCount == 1);
        EXPECT(s.columns[0] == kSelf && s.ListenerColumn() == -1);
    }
    {
        // A seat in but between members: their own view, as from the join; a seat outside the game is not in.
        const Shown s = Decide(At(kTwoPlayers, SeatState::In, SeatState::Awaiting));
        EXPECT(s.seats[1] == kSelf && s.seatIn[1] && !s.seatIn[2] && s.SplitShown());
        const Shown r = Decide(At(kTwoPlayers, SeatState::Rejoining, SeatState::Out));
        EXPECT(!r.seatIn[1] && !r.SplitShown());
    }
    static_assert(Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out)).SplitShown(), "constexpr");
}

/// A column no seat fills shows player 1's; a seat's column is read by its seat.
void Mirrors()
{
    {
        // The span held with player 1 alone: two mirrors, nothing single, no split of a further player.
        const Shown s = Decide(At(kOneAndMirrors, SeatState::Out, SeatState::Out, kHeld));
        EXPECT(!s.Single() && !s.SplitShown() && s.ViewsSplit() && s.columnCount == 3);
        EXPECT(s.columns[0] == kSelf && s.columns[1].why == Why::NoPlayer && s.columns[2].why == Why::NoPlayer);
        EXPECT(s.columns[1].source == Source::PlayerOne && s.ListenerColumn() == -1);
    }
    {
        // Two players and a mirror on the span: the player's own, the mirror player 1's; the listener on player 2's.
        const Shown s = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld));
        EXPECT(!s.Single() && s.SplitShown() && s.ViewsSplit() && s.columnCount == 3);
        EXPECT(s.columns[1] == kSelf && s.columns[2].why == Why::NoPlayer && s.ListenerColumn() == 1);
    }
    {
        // Player 3 in with player 2 out (column 1 is seat 2's), a mirror in column 2.
        const Shown s = Decide(At(kThreeOutOfOrder, SeatState::Out, SeatState::Playing, kHeld));
        EXPECT(s.columns[1] == kSelf && s.columns[2].why == Why::NoPlayer && s.ListenerColumn() == 1);
        EXPECT(s.seats[1] == kSelf && !s.seatIn[1]);  // out: no reason, not in
    }
    {
        // A list the mod cannot read (an unknown place): no one's column; an empty list: one column, player 1's.
        constexpr std::array<int, 2> unknown{0, -1};
        const Shown s = Decide(At(unknown, SeatState::Out, SeatState::Out));
        EXPECT(s.columns[1].why == Why::NoPlayer && s.columnCount == 2);
        const Shown e = Decide(At(std::array<int, 0>{}, SeatState::Out, SeatState::Out));
        EXPECT(e.columnCount == 1 && !e.ViewsSplit() && e.columns[0] == kSelf);
    }
}

/// The console's `single-view on|off` overrides the automatic choice while a further player is in.
void Overrides()
{
    {
        // On, one window: the engine's single view is unbuilt (the moment says), so every seat in goes to player 1's
        // picture in its own column (the mirror look), never Screen; with it built, Screen.
        const Shown s = Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out, false, Override::Single));
        EXPECT(!s.Single() && s.OnPlayerOne(1) && s.seats[1] == kColumnConsole && !s.SplitShown());
        EXPECT(s.ViewsSplit() && s.columnCount == 2 && s.ListenerColumn() == -1);
        const Shown three = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Playing, false, Override::Single));
        EXPECT(!three.Single() && three.OnPlayerOne(1) && three.OnPlayerOne(2) && !three.SplitShown());
        const Shown between = Decide(At(kThreePlayers, SeatState::In, SeatState::Playing, false, Override::Single));
        EXPECT(between.OnPlayerOne(1) && between.OnPlayerOne(2));  // a seat in but between members is in
        const Shown built = Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out, false, Override::Single, false, false, true));
        EXPECT(built.Single() && built.seats[1] == kScreenConsole && !built.SplitShown() && !built.OnPlayerOne(1));
        const Shown builtHeld = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld, Override::Single, false, false, true));
        EXPECT(!builtHeld.Single() && builtHeld.OnPlayerOne(1));  // columns held: never the engine's single view
        static_assert(!Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out, false, Override::Single)).Single(),
                      "never Screen while the single view is unbuilt");
    }
    {
        // On, the span with two seats: both columns on player 1's; three players, the same.
        const Shown s = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld, Override::Single));
        EXPECT(!s.Single() && s.OnPlayerOne(1) && s.columns[2].why == Why::NoPlayer && !s.SplitShown());
        const Shown t = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Handed, kHeld, Override::Single));
        EXPECT(t.OnPlayerOne(1) && t.OnPlayerOne(2) && t.seats[2].why == Why::Console && t.ListenerColumn() == -1);
    }
    {
        // Off: never player 1's for a seat; a mirror still shows it. Auto is the same today (no moment asks).
        const Shown s = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld, Override::Split, true));
        EXPECT(!s.Single() && s.seats[1] == kSelf && s.SplitShown() && s.columns[2].why == Why::NoPlayer);
        const Shown a = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld, Override::Auto, true));
        EXPECT(a == s);
    }
    {
        // On with no further player in: nothing (the game's own screen).
        const Shown s = Decide(At(kOnePlayer, SeatState::Out, SeatState::Out, false, Override::Single));
        EXPECT(!s.Single() && s.columns[0] == kSelf);
        const Shown a = Decide(At(kTwoPlayers, SeatState::Awaiting, SeatState::Out, false, Override::Single));
        EXPECT(!a.Single() && !a.seatIn[1]);
    }
    static_assert(Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld, Override::Single)).OnPlayerOne(1),
                  "constexpr");
}

bool Says(const std::wstring& text, const wchar_t* part)
{
    return text.find(part) != std::wstring::npos;
}

void Texts()
{
    EXPECT(Describe(1, kSelf, kSelf).empty());
    EXPECT(Says(Describe(1, kSelf, kColumnConsole), L"player 2's view shows player 1's camera and HUD: single-view on"));
    EXPECT(Says(Describe(2, kColumnConsole, kSelf), L"player 3's view is their own again"));
    EXPECT(Says(Describe(1, kSelf, kScreenConsole), L"not built"));
    for (const auto why : {Why::None, Why::Console, Why::NoPlayer})
    {
        EXPECT(NameOf(why)[0] != L'?');
    }
}

}  // namespace

int main()
{
    FieldPlay();
    Mirrors();
    Overrides();
    Texts();
    return lss::test::Report();
}
