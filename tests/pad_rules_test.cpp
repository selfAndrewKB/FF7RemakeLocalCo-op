// Host-side tests for each pad's pure input rules (players/pad_rules.h). Run without the game.

#include "players/pad_rules.h"

#include <cstdio>

namespace {

using namespace lss::players::pad_rules;

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

/// The table is a permutation; a user's slot is found.
void TheTable()
{
    EXPECT(IsPermutation(kIdentity));
    EXPECT(IsPermutation({2, 0, 1, 3}));
    EXPECT(!IsPermutation({0, 0, 1, 3}));
    EXPECT(!IsPermutation({0, 1, 2, 4}));
    EXPECT(SlotOf({2, 0, 1, 3}, 0) == 1);
    EXPECT(SlotOf({2, 0, 1, 3}, 3) == 3);
}

/// B is a tap or a hold, never both: a let-go within a second is the tap (at the let-go); a second down is the hold,
/// once, again only after a let-go, and that let-go is no tap.
void TheLeaveHold()
{
    Held h{};
    EXPECT(Hold(h, false, 0, kLeaveHoldMs) == Long::None);  // never down: no tap
    EXPECT(Hold(h, true, 0, kLeaveHoldMs) == Long::None);   // the press itself says nothing yet
    EXPECT(Hold(h, true, kLeaveHoldMs - 1, kLeaveHoldMs) == Long::None);
    EXPECT(Hold(h, true, kLeaveHoldMs, kLeaveHoldMs) == Long::Hold);
    EXPECT(Hold(h, true, kLeaveHoldMs + 500, kLeaveHoldMs) == Long::None);
    EXPECT(Hold(h, false, kLeaveHoldMs + 600, kLeaveHoldMs) == Long::None);  // the hold's let-go is no tap
    EXPECT(Hold(h, true, 5000, kLeaveHoldMs) == Long::None);
    EXPECT(Hold(h, true, 5000 + kLeaveHoldMs, kLeaveHoldMs) == Long::Hold);
    Held t{};
    EXPECT(Hold(t, true, 0, kLeaveHoldMs) == Long::None);
    EXPECT(Hold(t, true, 200, kLeaveHoldMs) == Long::None);
    EXPECT(Hold(t, false, 250, kLeaveHoldMs) == Long::Tap);  // let go within the second: the tap, once
    EXPECT(Hold(t, false, 300, kLeaveHoldMs) == Long::None);
}

/// A pad on a slot no player holds never pauses the game; player 1's slot always does.
void StartOnlyForPlayers()
{
    EXPECT(StartGate(kStart | kA, 0, 0b1) == (kStart | kA));
    EXPECT(StartGate(kStart | kA, 0, 0) == (kStart | kA));       // slot 0 whatever the mask
    EXPECT(StartGate(kStart | kA, 1, 0b1) == kA);                // no player 2: Start held back, A passes
    EXPECT(StartGate(kStart | kA, 1, 0b11) == (kStart | kA));
    EXPECT(StartGate(kStart, 3, 0b111) == 0);                    // the spare slot
}

/// View held a second opens a window; a shorter View press reaches the game at its let-go; other buttons pass.
void TheOpener()
{
    OpenHold h{};
    Gated    g = Gate(h, kView | kA, 0);
    EXPECT(g.buttons == kA && !g.open);  // View held back, A passes
    g = Gate(h, kView, kOpenHoldMs - 1);
    EXPECT(g.buttons == 0 && !g.open);
    g = Gate(h, kView, kOpenHoldMs);
    EXPECT(g.buttons == 0 && g.open);    // fires once
    g = Gate(h, kView, kOpenHoldMs + 500);
    EXPECT(g.buttons == 0 && !g.open);   // hidden until let go
    g = Gate(h, 0, kOpenHoldMs + 600);
    EXPECT(g.buttons == 0 && !g.open);   // the fired press is never a tap

    // A tap: the game gets View on the let-go poll, then up on the next.
    h = {};
    EXPECT(Gate(h, kView, 0).buttons == 0);
    EXPECT(Gate(h, kView, 90).buttons == 0);
    EXPECT(Gate(h, 0, 110).buttons == kView);
    EXPECT(Gate(h, 0, 126).buttons == 0);

    // Menu (Start) and the rest are never held back.
    h = {};
    EXPECT(Gate(h, kB | 0x0010, 0).buttons == (kB | 0x0010));

    // A new press after a fired one starts over.
    h = {};
    Gate(h, kView, 0);
    EXPECT(Gate(h, kView, kOpenHoldMs).open);
    Gate(h, 0, kOpenHoldMs + 10);
    EXPECT(!Gate(h, kView, 5000).open);
    EXPECT(Gate(h, kView, 5000 + kOpenHoldMs).open);
}
/// After a change a pad stays idle until it lets go of what it held then; a resting trigger or a stuck button never
/// holds it for good.
void TheLetGo()
{
    LetGo w{};
    EXPECT(Poll(w, kA, 0, 0, 0) == Wait::None);  // not waiting

    AwaitLetGo(w);
    EXPECT(Poll(w, kA, 0, 0, 0) == Wait::Holding);        // the press that closed a window
    EXPECT(Poll(w, kA | kB, 0, 0, 50) == Wait::Holding);  // a new press does not extend it
    EXPECT(Poll(w, kB, 0, 0, 100) == Wait::LetGoNow);     // A up: free (idle on this poll still)
    EXPECT(Poll(w, kB, 0, 0, 120) == Wait::None);

    AwaitLetGo(w);
    EXPECT(Poll(w, 0, 0, 0, 0) == Wait::LetGoNow);  // nothing held: free at once

    // A trigger pulled at the change lets go below the threshold, or when it drops by the threshold.
    AwaitLetGo(w);
    EXPECT(Poll(w, 0, 255, 0, 0) == Wait::Holding);
    EXPECT(Poll(w, 0, 240, 0, 10) == Wait::Holding);
    EXPECT(Poll(w, 0, 200, 0, 20) == Wait::LetGoNow);

    // A worn trigger resting at 60: it never drops; 2 s after the buttons let go the pad is free anyway.
    AwaitLetGo(w);
    EXPECT(Poll(w, kA, 60, 0, 0) == Wait::Holding);
    EXPECT(Poll(w, 0, 60, 0, 100) == Wait::Holding);
    EXPECT(Poll(w, 0, 61, 0, 100 + kLetGoCapMs - 1) == Wait::Holding);
    EXPECT(Poll(w, 0, 60, 0, 100 + kLetGoCapMs) == Wait::TriggerRests);
    EXPECT(Poll(w, 0, 60, 0, 100 + kLetGoCapMs + 16) == Wait::None);

    // A stuck button bit, down at the change and never up: free 10 s after the change.
    AwaitLetGo(w);
    EXPECT(Poll(w, kY, 0, 0, 1000) == Wait::Holding);
    EXPECT(Poll(w, kY, 0, 0, 1000 + kLetGoButtonCapMs - 1) == Wait::Holding);
    EXPECT(Poll(w, kY, 0, 0, 1000 + kLetGoButtonCapMs) == Wait::ButtonStuck);
    EXPECT(Poll(w, kY, 0, 0, 1000 + kLetGoButtonCapMs + 16) == Wait::None);
}

}  // namespace

int main()
{
    TheTable();
    TheOpener();
    TheLetGo();
    TheLeaveHold();
    StartOnlyForPlayers();
    if (g_failures != 0)
    {
        std::printf("pad_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("pad_rules: all passed\n");
    return 0;
}
