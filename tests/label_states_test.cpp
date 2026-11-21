// Host-side tests for hud::label_states::LabelBook, run without the game.
//
// A fake widget's "state" is a number standing for its layout (0: the empty state the constructor writes). The cases:
// the first pass lays the seat's state out from the fresh state; later passes swap the seat's own state in and player
// 1's back; a second swap-in in one pass swaps nothing; a changed stamp resets the seat's state; a widget dying inside
// the seat's pass or aside; releasing everything; refusals, a full table and faults; the weak references balance.

#include "hud/label_states.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using lss::hud::label_states::EmptyState;
using lss::hud::label_states::LabelAccess;
using lss::hud::label_states::LabelBook;
using lss::hud::label_states::Parked;

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

struct FakeWidget
{
    int           state      = 0;
    bool          alive      = true;
    bool          usable     = true;
    bool          swapFaults = false;
    std::uint64_t stamp      = 7;
    int           weak       = 1;  // its own WeakThis
    // The controller is the widget's address here; the book only compares and passes it.
};

std::vector<int> g_freed;  // states freed from a set-aside copy (0 = an empty one)

int StateOf(const Parked& parked)
{
    int state = 0;
    std::memcpy(&state, parked.runs, sizeof(state));
    return state;
}

void SetState(Parked& parked, int state)
{
    std::memcpy(parked.runs, &state, sizeof(state));
}

FakeWidget* AsWidget(void* p)
{
    return static_cast<FakeWidget*>(p);
}

bool Usable(void* w) noexcept { return AsWidget(w)->usable && AsWidget(w)->alive; }
bool Alive(void* c) noexcept { return AsWidget(c)->alive; }
bool Swap(void* w, Parked& parked) noexcept
{
    FakeWidget* widget = AsWidget(w);
    if (widget->swapFaults)
    {
        return false;
    }
    const int aside = StateOf(parked);
    SetState(parked, widget->state);
    widget->state = aside;
    return true;
}
std::uint64_t Stamp(void* w) noexcept { return AsWidget(w)->stamp; }
bool          HoldWeak(void* c) noexcept
{
    ++AsWidget(c)->weak;
    return true;
}
void ReleaseWeak(void* c) noexcept { --AsWidget(c)->weak; }
void FreeParked(Parked& parked) noexcept
{
    g_freed.push_back(StateOf(parked));
    parked = EmptyState();
}

constexpr LabelAccess kAccess{&Usable, &Alive, &Swap, &Stamp, &HoldWeak, &ReleaseWeak, &FreeParked};

LabelBook Fresh()
{
    g_freed.clear();
    return LabelBook(kAccess);
}

// One seat pass: the seat's state in, the game's relayout writes `p2` if the state in is not it yet (a new or reset
// state), the paint. Player 1's state is in the widget before and after.
void SeatPass(LabelBook& book, FakeWidget& w, int p2)
{
    EXPECT(book.SwapIn(&w, &w));
    if (w.state != p2)
    {
        w.state = p2;  // the game's relayout, into whichever state is in the widget
    }
}

void EmptyStateLayout()
{
    const Parked empty     = EmptyState();
    float        shadow[2] = {};
    std::memcpy(shadow, empty.quads + lss::hud::label_states::kShadowInQuads, sizeof(shadow));
    EXPECT(shadow[0] == 1.0f && shadow[1] == 1.0f);
    EXPECT(StateOf(empty) == 0);
    for (std::size_t i = 0; i < lss::hud::label_states::kShadowInQuads; ++i)
    {
        EXPECT(empty.quads[i] == std::byte{0});
    }
    EXPECT(lss::hud::label_states::kShadowInQuads == 0x30);  // the quads' shadow offset cache, after three arrays
}

void FirstPassThenOwnStates()
{
    auto       book = Fresh();
    FakeWidget w;
    w.state = 1;  // player 1's
    EXPECT(book.SwapIn(&w, &w));
    EXPECT(w.state == 0);  // the fresh state: the relayout builds the seat's
    EXPECT(book.AnyInside());
    w.state = 2;  // the relayout
    book.SwapBack();
    EXPECT(w.state == 1 && !book.AnyInside());
    EXPECT(w.weak == 2 && book.Count() == 1);

    SeatPass(book, w, 2);
    EXPECT(w.state == 2);  // the seat's own state, already laid out
    book.SwapBack();
    EXPECT(w.state == 1);
    EXPECT(book.Counts().created == 1 && book.Counts().swapsIn == 2 && book.Counts().swapsOut == 2);
    EXPECT(g_freed.empty());
}

void SecondSwapInOnePass()
{
    auto       book = Fresh();
    FakeWidget w;
    w.state = 1;
    EXPECT(book.SwapIn(&w, &w));
    w.state = 2;
    EXPECT(book.SwapIn(&w, &w));
    EXPECT(w.state == 2 && book.Counts().swapsIn == 1);
    book.SwapBack();
    EXPECT(w.state == 1);
}

void StampChangeResets()
{
    auto       book = Fresh();
    FakeWidget w;
    w.state = 1;
    SeatPass(book, w, 2);
    book.SwapBack();
    w.stamp = 8;  // the text, a font or the seat's art changed
    EXPECT(book.SwapIn(&w, &w));
    EXPECT(w.state == 0);
    EXPECT(g_freed.size() == 1 && g_freed[0] == 2);
    EXPECT(book.Counts().resets == 1);
    w.state = 3;
    book.SwapBack();
    EXPECT(w.state == 1);
    SeatPass(book, w, 3);
    EXPECT(w.state == 3 && book.Counts().resets == 1);  // the new stamp is kept
    book.SwapBack();
}

void FreshState()
{
    g_freed.clear();
    Parked fresh = EmptyState();
    SetState(fresh, 9);
    LabelBook  book(kAccess, fresh);
    FakeWidget w;
    w.state = 1;
    EXPECT(book.SwapIn(&w, &w));
    EXPECT(w.state == 9);
    w.state = 3;
    book.SwapBack();
    EXPECT(w.state == 1);
    w.stamp = 8;
    EXPECT(book.SwapIn(&w, &w));
    EXPECT(w.state == 9 && g_freed.size() == 1 && g_freed[0] == 3);
    book.SwapBack();
    book.ReleaseAll();
}

void StampedAsItGoesOut()
{
    auto       book = Fresh();
    FakeWidget w;
    w.state = 1;
    w.stamp = 1;  // player 1's text T1
    EXPECT(book.SwapIn(&w, &w));
    w.state = 2;
    w.stamp = 2;  // the pass sets T2, laid out into the seat's state
    book.SwapBack();
    w.stamp = 1;  // player 1's pass sets T1 again
    EXPECT(book.SwapIn(&w, &w));
    EXPECT(w.state == 0 && book.Counts().resets == 1);  // laid out afresh: the seat's T2 layout is not shown for T1
    book.SwapBack();
    book.ReleaseAll();
}

void DiesInsideTheSeatsPass()
{
    auto       book = Fresh();
    FakeWidget w;
    w.state = 1;
    SeatPass(book, w, 2);
    w.alive = false;  // its destructor freed the seat's state, in the widget
    book.SwapBack();
    EXPECT(w.state == 2);  // untouched
    EXPECT(g_freed.size() == 1 && g_freed[0] == 1);
    EXPECT(w.weak == 1 && book.Count() == 0 && book.Counts().diedInside == 1);
}

void DiesAside()
{
    auto       book = Fresh();
    FakeWidget w;
    w.state = 1;
    SeatPass(book, w, 2);
    book.SwapBack();
    book.Sweep();
    EXPECT(book.Count() == 1 && g_freed.empty());
    w.alive = false;
    book.Sweep();
    EXPECT(g_freed.size() == 1 && g_freed[0] == 2);
    EXPECT(w.weak == 1 && book.Count() == 0 && book.Counts().diedAside == 1);
}

void MiddleRecordDropped()
{
    auto       book = Fresh();
    FakeWidget a, b, c;
    a.state = 1;
    b.state = 11;
    c.state = 21;
    SeatPass(book, a, 2);
    SeatPass(book, b, 12);
    SeatPass(book, c, 22);
    b.alive = false;  // dies inside the seat's pass
    book.SwapBack();
    EXPECT(a.state == 1 && c.state == 21 && b.state == 12);
    EXPECT(g_freed.size() == 1 && g_freed[0] == 11);
    EXPECT(book.Count() == 2 && b.weak == 1 && a.weak == 2 && c.weak == 2);

    SeatPass(book, a, 2);  // both survivors still find their own records
    SeatPass(book, c, 22);
    EXPECT(a.state == 2 && c.state == 22 && book.Counts().created == 3);
    book.SwapBack();
    a.alive = false;  // dies aside
    book.Sweep();
    EXPECT(c.state == 21 && book.Count() == 1 && a.weak == 1 && c.weak == 2);
    EXPECT(g_freed.size() == 2 && g_freed[1] == 2);
    SeatPass(book, c, 22);
    EXPECT(c.state == 22);
    book.ReleaseAll();
    EXPECT(c.state == 21 && c.weak == 1 && g_freed.size() == 3 && g_freed[2] == 22);
}

void ReleaseAll()
{
    auto       book = Fresh();
    FakeWidget a, b;
    a.state = 1;
    b.state = 11;
    SeatPass(book, a, 2);
    SeatPass(book, b, 12);
    book.ReleaseAll();  // even from inside the pass
    EXPECT(a.state == 1 && b.state == 11);
    EXPECT(g_freed.size() == 2);
    EXPECT(a.weak == 1 && b.weak == 1 && book.Count() == 0 && book.Counts().released == 2);
}

void RefusalsAndFull()
{
    auto       book = Fresh();
    FakeWidget odd;
    odd.state  = 5;
    odd.usable = false;
    EXPECT(book.SwapIn(&odd, &odd));
    EXPECT(odd.state == 5 && odd.weak == 1 && book.Count() == 0 && book.Counts().refused == 1);

    std::vector<FakeWidget> many(LabelBook::kMaxLabels + 1);
    for (FakeWidget& w : many)
    {
        w.state = 1;
        EXPECT(book.SwapIn(&w, &w));
    }
    EXPECT(book.Count() == LabelBook::kMaxLabels && book.Counts().full == 1);
    EXPECT(many.back().state == 1 && many.back().weak == 1);
    book.ReleaseAll();
    for (const FakeWidget& w : many)
    {
        EXPECT(w.state == 1 && w.weak == 1);
    }
}

void Faults()
{
    auto       book = Fresh();
    FakeWidget w;
    w.state      = 1;
    w.swapFaults = true;
    EXPECT(!book.SwapIn(&w, &w));
    EXPECT(w.state == 1 && !book.AnyInside() && book.Counts().faults == 1);
    book.ReleaseAll();
    EXPECT(w.weak == 1);

    auto       again = Fresh();
    FakeWidget v;
    v.state = 1;
    SeatPass(again, v, 2);
    v.swapFaults = true;
    again.SwapBack();
    EXPECT(v.state == 2);  // the game's next relayout for player 1 lays player 1's out again
    EXPECT(g_freed.size() == 1 && g_freed[0] == 1);
    EXPECT(v.weak == 1 && again.Count() == 0 && again.Counts().faults == 1);
}

}  // namespace

int main()
{
    EmptyStateLayout();
    FirstPassThenOwnStates();
    SecondSwapInOnePass();
    StampChangeResets();
    FreshState();
    StampedAsItGoesOut();
    DiesInsideTheSeatsPass();
    DiesAside();
    MiddleRecordDropped();
    ReleaseAll();
    RefusalsAndFull();
    Faults();
    if (g_failures != 0)
    {
        std::printf("label_states: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("label_states: all passed\n");
    return 0;
}
