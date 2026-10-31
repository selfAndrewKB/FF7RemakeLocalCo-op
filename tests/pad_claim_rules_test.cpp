// Host-side tests for the live pad assignment's pure rules (players/pad_claim_rules.h). Run without the game.

#include "players/pad_claim_rules.h"

#include <cstdio>

namespace {

using namespace lss::players::pad_claim_rules;

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

/// No player on two screens, the players shown the same as at the opening, and the pad table a permutation: what every
/// swap keeps.
bool Whole(const Claim& c)
{
    for (int seat = 0; seat < kScreens; ++seat)
    {
        int shown = 0, opened = 0;
        for (int s = 0; s < c.screens; ++s)
        {
            shown += c.seatOn[static_cast<std::size_t>(s)] == seat ? 1 : 0;
            opened += c.openSeatOn[static_cast<std::size_t>(s)] == seat ? 1 : 0;
        }
        if (shown > 1 || shown != opened)
        {
            return false;
        }
    }
    return IsPermutation(c.pads);
}

/// The user's case (2026-10-30 18:43): person C sits at the middle screen, which shows Tifa (player 3); C wants Barret
/// (player 2, on the right). C's pad is XInput user 0 (Steam's order).
void TheMiddleScreenWantsBarret()
{
    Claim c = Open(3, {0, 2, 1}, kIdentity);
    EXPECT(Whole(c));

    // C presses X, the middle screen's button: C's pad answers Tifa's slot; the pad there goes to C's old slot.
    Result r = Press(c, 0, kX);
    EXPECT(r.did == Did::Took && r.screen == 1);
    EXPECT(c.pads[2] == 0 && c.pads[0] == 2);
    EXPECT(Whole(c));

    // C steps left (the party order backwards from Tifa): Barret comes to the middle screen with C's pad.
    r = Press(c, 0, kDpadLeft);
    EXPECT(r.did == Did::Stepped);
    EXPECT(c.seatOn[1] == 1 && c.seatOn[2] == 2);
    EXPECT(c.pads[1] == 0);   // C's pad drives Barret
    EXPECT(c.pads[2] == 1);   // the right screen's person now has Tifa, with the pad that had Barret
    EXPECT(Whole(c));

    r = Press(c, 0, kA);
    EXPECT(r.did == Did::Kept && !Done(c));

    // The right screen's person takes it (Y) and keeps it: the last screen, Cloud's, takes what is left.
    EXPECT(Press(c, 1, kY).did == Did::Took);
    EXPECT(c.pads[2] == 1);  // already that player's pad: no change
    EXPECT(Press(c, 1, kA).did == Did::Kept);
    EXPECT(Done(c));
    EXPECT(c.pads[0] == 2 && c.pads[1] == 0 && c.pads[2] == 1 && c.pads[3] == 3);
}

/// A screen taken by one pad is not another's; a mirror's screen is no one's; a pad not on a screen only takes.
void TakingIsExclusive()
{
    Claim c = Open(3, {0, 1, -1}, kIdentity);
    EXPECT(Press(c, 3, kX).did == Did::Took);    // the spare pad (user 3) takes player 2's screen
    EXPECT(c.pads[1] == 3 && c.pads[3] == 1);    // player 2's old pad goes spare
    EXPECT(Press(c, 1, kX).did == Did::Nothing); // taken
    EXPECT(Press(c, 1, kY).did == Did::Nothing); // a mirror's screen
    EXPECT(Press(c, 1, kA).did == Did::Took);    // Cloud's screen
    EXPECT(Press(c, 2, kDpadRight).did == Did::Nothing);  // user 2 is on no screen
    EXPECT(Whole(c));
}

/// Player 1's view moves like the others: a step from it or toward it swaps screens; with two screens the two swap.
void PlayerOneMoves()
{
    Claim c = Open(3, {0, 1, 2}, kIdentity);
    EXPECT(Press(c, 0, kA).did == Did::Took);
    EXPECT(Press(c, 0, kDpadRight).did == Did::Stepped);  // the first screen's person now has Barret
    EXPECT(c.seatOn[0] == 1 && c.seatOn[1] == 0 && c.pads[1] == 0 && c.pads[0] == 1);
    EXPECT(Whole(c));

    Claim two = Open(2, {0, 1, 2}, kIdentity);
    EXPECT(two.seatOn[2] == -1);  // beyond the screens: none
    EXPECT(Press(two, 1, kX).did == Did::Took);
    EXPECT(Press(two, 1, kDpadLeft).did == Did::Stepped);
    EXPECT(two.seatOn[1] == 0 && two.seatOn[0] == 1 && two.pads[0] == 1 && two.pads[1] == 0);
    EXPECT(Whole(two));
}

/// A kept screen is skipped by other people's steps; B unkeeps, then lets go; the combination again puts all back.
void KeepBackAndCancel()
{
    Claim c = Open(3, {0, 1, 2}, kIdentity);
    EXPECT(Press(c, 1, kX).did == Did::Took);
    EXPECT(Press(c, 1, kA).did == Did::Kept);
    EXPECT(Press(c, 2, kY).did == Did::Took);
    EXPECT(Press(c, 2, kDpadLeft).did == Did::Stepped);   // player 2's screen is kept: the step skips it to Cloud
    EXPECT(c.seatOn[2] == 0 && c.seatOn[0] == 2 && c.seatOn[1] == 1);
    EXPECT(Press(c, 1, kDpadRight).did == Did::Nothing);  // a kept screen does not step
    EXPECT(Press(c, 1, kB).did == Did::Unkept);
    EXPECT(Press(c, 2, kDpadRight).did == Did::Stepped);  // Barret's screen is free again: right from Cloud is Barret
    EXPECT(c.seatOn[2] == 1 && c.seatOn[1] == 0);
    EXPECT(Press(c, 2, kB).did == Did::LetGo);
    EXPECT(ScreenOf(c, 2) < 0);
    EXPECT(Whole(c));

    Cancel(c);
    EXPECT(c.seatOn == (std::array<int, kScreens>{0, 1, 2}) && c.pads == kIdentity);
    EXPECT(ScreenOf(c, 1) < 0 && !Done(c));
}

/// One screen: the claim ends at its keep. Two screens: one keep ends it.
void WhenItEnds()
{
    Claim one = Open(1, {0, 1, 2}, {1, 0, 2, 3});
    EXPECT(Press(one, 0, kA).did == Did::Took);
    EXPECT(one.pads[0] == 0 && one.pads[1] == 1);
    EXPECT(!Done(one));
    EXPECT(Press(one, 0, kA).did == Did::Kept && Done(one));

    Claim two = Open(2, {0, 1, 2}, kIdentity);
    EXPECT(!Done(two));
    EXPECT(Press(two, 0, kX).did == Did::Took);
    EXPECT(Press(two, 0, kA).did == Did::Kept && Done(two));
    EXPECT(two.pads[1] == 0 && two.pads[0] == 1);
}

/// Any run of presses keeps every player on one screen and the table a permutation.
void EverySwapKeepsTheWhole()
{
    constexpr std::uint16_t kButtons[] = {kA, kB, kX, kY, kDpadLeft, kDpadRight};
    unsigned                seed       = 12345;
    for (int run = 0; run < 200; ++run)
    {
        Claim c = Open(3, {0, 2, 1}, {3, 1, 0, 2});
        for (int i = 0; i < 60; ++i)
        {
            seed = seed * 1103515245u + 12345u;
            Press(c, static_cast<int>((seed >> 8) % kSlots), kButtons[(seed >> 16) % 6]);
            EXPECT(Whole(c));
            for (int s = 0; s < c.screens; ++s)
            {
                const int user = c.takenBy[static_cast<std::size_t>(s)];
                if (user >= 0)  // a taken screen's player is answered by the pad that took it
                {
                    EXPECT(c.pads[static_cast<std::size_t>(c.seatOn[static_cast<std::size_t>(s)])] == user);
                }
            }
        }
    }
}

/// View held a second opens the claim; a shorter View press reaches the game at its let-go; other buttons pass.
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
    EXPECT(Poll(w, kA, 0, 0, 0) == Wait::Holding);        // the press that ended the claim
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
    TheMiddleScreenWantsBarret();
    TakingIsExclusive();
    PlayerOneMoves();
    KeepBackAndCancel();
    WhenItEnds();
    EverySwapKeepsTheWhole();
    TheOpener();
    TheLetGo();
    if (g_failures != 0)
    {
        std::printf("pad_claim_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("pad_claim_rules: all passed\n");
    return 0;
}
