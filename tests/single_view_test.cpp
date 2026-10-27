// Host-side tests for players/single_view.h, run without the game (Rebirth's tests/single_view_test.cpp, the same
// rules). The switch is a fake bool. The cases: nothing happens while the single view is not wanted; the mod sets
// the switch and clears only its own; a switch found set is never cleared; a switch cleared by someone else is not
// written again; failures reported once per request and retried when the request changes. Which moments want the
// single view is views/column_source.h's (tests/column_source_test.cpp).

#include "players/single_view.h"

#include <cstdio>
#include <optional>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "single_view: all passed\n" : "single_view: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
}  // namespace lss::test

#define EXPECT(condition)                                                \
    do                                                                   \
    {                                                                    \
        if (!(condition))                                                \
        {                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition); \
            ++lss::test::g_failures;                                     \
        }                                                                \
    } while (false)

namespace {

using lss::players::SingleView;
using lss::players::SingleViewChange;
using lss::players::SplitSwitchAccess;

struct FakeSwitch
{
    bool disabled   = false;
    bool readWorks  = true;
    bool writeWorks = true;
    int  writes     = 0;
} g_switch;

std::optional<bool> Read() noexcept
{
    return g_switch.readWorks ? std::optional<bool>(g_switch.disabled) : std::nullopt;
}

bool Write(bool disabled) noexcept
{
    ++g_switch.writes;
    if (!g_switch.writeWorks)
    {
        return false;
    }
    g_switch.disabled = disabled;
    return true;
}

SingleView Fresh()
{
    g_switch = FakeSwitch{};
    return SingleView(SplitSwitchAccess{&Read, &Write});
}

/// Not wanted: nothing is read or written, tick after tick.
void NotWanted()
{
    SingleView view = Fresh();
    EXPECT(view.Update(false) == SingleViewChange::None);
    EXPECT(view.Update(false) == SingleViewChange::None);
    EXPECT(!view.Single() && g_switch.writes == 0 && !g_switch.disabled);
}

/// Wanted: the switch set once, held through the ticks, cleared once when no longer wanted.
void SetAndCleared()
{
    SingleView view = Fresh();
    EXPECT(view.Update(true) == SingleViewChange::Single);
    EXPECT(view.Single() && g_switch.disabled && g_switch.writes == 1);
    EXPECT(view.Update(true) == SingleViewChange::None);
    EXPECT(g_switch.writes == 1);
    EXPECT(view.Update(false) == SingleViewChange::Split);
    EXPECT(!view.Single() && !g_switch.disabled && g_switch.writes == 2);
    EXPECT(view.Update(false) == SingleViewChange::None);
}

/// A switch found set is used and never cleared by the mod.
void FoundSet()
{
    SingleView view   = Fresh();
    g_switch.disabled = true;
    EXPECT(view.Update(true) == SingleViewChange::AlreadySingle);
    EXPECT(view.Single() && g_switch.writes == 0);
    EXPECT(view.Update(false) == SingleViewChange::LeftAlone);
    EXPECT(!view.Single() && g_switch.disabled && g_switch.writes == 0);
}

/// A switch someone cleared meanwhile is not written again.
void ClearedMeanwhile()
{
    SingleView view = Fresh();
    EXPECT(view.Update(true) == SingleViewChange::Single);
    g_switch.disabled = false;
    EXPECT(view.Update(false) == SingleViewChange::LeftAlone);
    EXPECT(!view.Single() && g_switch.writes == 1);
}

/// Failures: said once per request, not retried until the request changes; a write that does not take is a failure.
void Failures()
{
    SingleView view    = Fresh();
    g_switch.readWorks = false;
    EXPECT(view.Update(true) == SingleViewChange::SingleFailed);
    EXPECT(view.Update(true) == SingleViewChange::None);
    EXPECT(!view.Single() && g_switch.writes == 0);
    EXPECT(view.Update(false) == SingleViewChange::None);  // not single: nothing to clear
    g_switch.readWorks = true;
    EXPECT(view.Update(true) == SingleViewChange::Single);  // the request changed: tried again

    view                = Fresh();
    g_switch.writeWorks = false;
    EXPECT(view.Update(true) == SingleViewChange::SingleFailed);
    EXPECT(!view.Single() && g_switch.writes == 1);
    EXPECT(view.Update(true) == SingleViewChange::None);
    EXPECT(g_switch.writes == 1);

    view = Fresh();
    EXPECT(view.Update(true) == SingleViewChange::Single);
    g_switch.writeWorks = false;
    EXPECT(view.Update(false) == SingleViewChange::SplitFailed);
    EXPECT(view.Single() && g_switch.disabled);
    EXPECT(view.Update(false) == SingleViewChange::None);
    g_switch.writeWorks = true;
    EXPECT(view.Update(true) == SingleViewChange::None);   // wanted again while still single: nothing to do
    EXPECT(view.Update(false) == SingleViewChange::Split);  // the request changed: cleared
    EXPECT(!view.Single() && !g_switch.disabled);
}

/// The release at the end: the mod's switch cleared even after a failed clear; nothing else's.
void ReleaseAtEnd()
{
    SingleView view = Fresh();
    EXPECT(view.Release() == SingleViewChange::None);
    EXPECT(view.Update(true) == SingleViewChange::Single);
    g_switch.writeWorks = false;
    EXPECT(view.Update(false) == SingleViewChange::SplitFailed);
    g_switch.writeWorks = true;
    EXPECT(view.Release() == SingleViewChange::Split);
    EXPECT(!view.Single() && !g_switch.disabled);
    EXPECT(view.Release() == SingleViewChange::None);

    view              = Fresh();
    g_switch.disabled = true;
    EXPECT(view.Update(true) == SingleViewChange::AlreadySingle);
    EXPECT(view.Release() == SingleViewChange::None);  // not the mod's: nothing to clear
    EXPECT(g_switch.disabled && g_switch.writes == 0);
}

/// Every change but None has words.
void Texts()
{
    EXPECT(lss::players::Describe(SingleViewChange::None).empty());
    for (const auto change : {SingleViewChange::Single, SingleViewChange::AlreadySingle, SingleViewChange::SingleFailed,
                              SingleViewChange::Split, SingleViewChange::LeftAlone, SingleViewChange::SplitFailed})
    {
        EXPECT(!lss::players::Describe(change).empty());
    }
}

}  // namespace

int main()
{
    NotWanted();
    SetAndCleared();
    FoundSet();
    ClearedMeanwhile();
    Failures();
    ReleaseAtEnd();
    Texts();
    return lss::test::Report();
}
