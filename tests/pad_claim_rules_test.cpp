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

/// The user's case (2026-10-30 18:43): person C sits at the middle screen, which shows player 3 (Tifa); C wants to
/// play there. C's pad is XInput user 0 (Steam's order). The screens stay where they are; the character steps by rank.
void TheMiddleScreenTakesItsPlayer()
{
    Claim c = Open(3, {0, 2, 1}, kIdentity);
    EXPECT(Whole(c));

    // C presses X, the middle screen's button: C's pad answers player 3's slot; the pad there goes to C's old slot.
    Result r = Press(c, 0, kX);
    EXPECT(r.did == Did::Took && r.screen == 1);
    EXPECT(c.pads[2] == 0 && c.pads[0] == 2);
    EXPECT(Whole(c));

    // C steps left: the claim asks for the next member; the screens and pads stay.
    r = Press(c, 0, kDpadLeft);
    EXPECT(r.did == Did::Step && r.screen == 1 && r.dir == -1);
    EXPECT(c.seatOn == (std::array<int, kScreens>{0, 2, 1}) && c.pads[2] == 0);
    EXPECT(Press(c, 0, kDpadRight).dir == 1);

    r = Press(c, 0, kA);
    EXPECT(r.did == Did::Kept && !Done(c));

    // The right screen's person takes it (Y) and keeps it: the last screen, player 1's, takes what is left.
    EXPECT(Press(c, 1, kY).did == Did::Took);
    EXPECT(c.pads[1] == 1);  // already that player's pad: no change
    EXPECT(Press(c, 1, kA).did == Did::Kept);
    EXPECT(Done(c));
    EXPECT(c.pads[0] == 2 && c.pads[1] == 1 && c.pads[2] == 0 && c.pads[3] == 3);
}

/// A screen taken by one pad is not another's; a mirror's screen is no one's; a pad not on a screen only takes.
void TakingIsExclusive()
{
    Claim c = Open(3, {0, 1, -1}, kIdentity);
    EXPECT(Press(c, 3, kX).did == Did::Took);    // the spare pad (user 3) takes player 2's screen
    EXPECT(c.pads[1] == 3 && c.pads[3] == 1);    // player 2's old pad goes spare
    EXPECT(Press(c, 1, kX).did == Did::Nothing); // taken
    EXPECT(Press(c, 1, kY).did == Did::Join);    // a mirror's screen: the spare pad joins there
    EXPECT(Press(c, 1, kA).did == Did::Nothing); // a pad whose join is under way waits for it
    EXPECT(Press(c, 2, kA).did == Did::Took);    // player 1's screen
    EXPECT(Press(c, 2, kDpadRight).did == Did::Nothing);  // user 2 is on no screen
    EXPECT(Whole(c));
}

/// Player 1's character is the game's leader: its screen does not step; any pad may still take player 1's screen.
void PlayerOneDoesNotStep()
{
    Claim c = Open(3, {0, 1, 2}, kIdentity);
    EXPECT(Press(c, 2, kA).did == Did::Took);  // pad 3 becomes player 1
    EXPECT(c.pads[0] == 2 && c.pads[2] == 0);
    EXPECT(Press(c, 2, kDpadRight).did == Did::Nothing);
    EXPECT(Press(c, 2, kDpadLeft).did == Did::Nothing);
    EXPECT(c.seatOn == (std::array<int, kScreens>{0, 1, 2}));
    EXPECT(Whole(c));

    Claim two = Open(2, {0, 1, 2}, kIdentity);
    EXPECT(two.seatOn[2] == -1);  // beyond the screens: none
    EXPECT(Press(two, 1, kX).did == Did::Took);
    EXPECT(Press(two, 1, kDpadLeft).did == Did::Step);
    EXPECT(two.seatOn[1] == 1 && two.pads[1] == 1);
}

/// A kept screen does not step; B unkeeps, then lets go; the opener again puts the pads back.
void KeepBackAndCancel()
{
    Claim c = Open(3, {0, 1, 2}, kIdentity);
    EXPECT(Press(c, 0, kX).did == Did::Took);  // pad 1 takes player 2's screen
    EXPECT(Press(c, 0, kA).did == Did::Kept);
    EXPECT(Press(c, 0, kDpadRight).did == Did::Nothing);  // a kept screen does not step
    EXPECT(Press(c, 0, kB).did == Did::Unkept);
    EXPECT(Press(c, 0, kDpadRight).did == Did::Step);
    EXPECT(Press(c, 0, kB).did == Did::LetGo);
    EXPECT(ScreenOf(c, 0) < 0);
    EXPECT(c.pads[1] == 0);  // let go: the pad stays that player's until the claim is put back
    EXPECT(Whole(c));

    Cancel(c);
    EXPECT(c.seatOn == (std::array<int, kScreens>{0, 1, 2}) && c.pads == kIdentity);
    EXPECT(ScreenOf(c, 1) < 0 && !Done(c));
}

/// The night's case (2026-11-02): player 1 alone, copies on the other two monitors. A spare pad joins on a copy's
/// screen with that screen's button; player 1's pad cannot; Start joins on the first open screen.
void JoiningFromTheClaim()
{
    // Player 1 on the middle monitor (order 2 1 3), copies left and right; pad 1 plays player 1, pads 2-4 spare.
    Claim c = Open(3, {-1, 0, -1}, kIdentity, 0b1);
    EXPECT(Spare(c, 1) && Spare(c, 3) && !Spare(c, 0));
    EXPECT(Press(c, 0, kA).did == Did::Nothing);       // player 1's pad on an open screen: nothing
    Result r = Press(c, 1, kA);                         // pad 2 on the left copy's button
    EXPECT(r.did == Did::Join && r.screen == 0 && c.joiningBy[0] == 1);
    EXPECT(Press(c, 1, kY).did == Did::Nothing);       // one join per pad
    EXPECT(Press(c, 2, kA).did == Did::Nothing);       // that screen is being joined
    r = Press(c, 2, kStart);                            // pad 3's Start: the first open screen left, the right one
    EXPECT(r.did == Did::Join && r.screen == 2 && c.joiningBy[2] == 2);
    EXPECT(!Done(c));                                   // joins under way
    r = Press(c, 3, kStart);                            // no open screen left: a new view (the caller refuses on a row)
    EXPECT(r.did == Did::Join && r.screen == -1);
    EXPECT(Press(c, 0, kStart).did == Did::Nothing);   // player 1's Start never joins
    EXPECT(IsPermutation(c.pads));

    // One window, player 1 alone: Start asks for a new view.
    Claim one = Open(1, {0, -1, -1}, kIdentity, 0b1);
    r         = Press(one, 1, kStart);
    EXPECT(r.did == Did::Join && r.screen == -1);
}

/// After a join the claim goes on: each player's take and keep carried by player, a join still under way carried,
/// the opening re-based so a cancel keeps the join.
void TheRebase()
{
    Claim c = Open(3, {-1, 0, -1}, kIdentity, 0b1);
    EXPECT(Press(c, 0, kX).did == Did::Took);  // pad 1 takes player 1's screen (the middle: X)
    EXPECT(Press(c, 0, kA).did == Did::Kept);
    EXPECT(Press(c, 1, kA).did == Did::Join);  // pad 2 joins on the left
    EXPECT(Press(c, 2, kY).did == Did::Join);  // pad 3 joins on the right
    // Player 2 is in on the left; player 3's join still runs.
    Claim n = Rebase(c, 3, {1, 0, -1}, kIdentity, 0b11);
    EXPECT(n.takenBy[1] == 0 && n.kept[1]);     // player 1's take and keep, carried
    EXPECT(n.takenBy[0] < 0 && !n.kept[0]);     // the new player's screen: the caller gives it to its pad
    EXPECT(n.joiningBy[2] == 2 && n.joiningBy[0] < 0);
    EXPECT(!Done(n));
    EXPECT(n.openSeatOn == (std::array<int, kScreens>{1, 0, -1}));
    n.takenBy[0] = 1;
    EXPECT(Press(n, 1, kDpadRight).did == Did::Step);  // the joiner steps its character
    EXPECT(Press(n, 1, kA).did == Did::Kept);
    EXPECT(!Done(n));                                  // the right screen is still being joined
    n.joiningBy[2] = -1;                               // that join refused: the screen stays open
    EXPECT(Done(n));                                   // every player kept, a screen open: done
    Cancel(n);
    EXPECT(n.seatOn == (std::array<int, kScreens>{1, 0, -1}));  // the join stays
}

/// The claim opened by a spare pad's Start: everyone in kept, the joiner alone chooses.
void OpenedByAJoin()
{
    Claim c = Open(2, {0, 1, -1}, {0, 1, 2, 3}, 0b11);
    KeepAll(c);
    EXPECT(c.kept[0] && c.kept[1] && c.takenBy[0] == 0 && c.takenBy[1] == 1);
    EXPECT(Done(c));  // nothing to choose yet: the caller holds it open while the join runs
    EXPECT(Press(c, 2, kStart).did == Did::Join);
    EXPECT(Press(c, 1, kB).did == Did::Unkept);  // anyone may still change
}

/// B held a second: a further player's pad takes that player out; player 1's is refused; a spare pad's does nothing.
void Leaving()
{
    Claim c = Open(3, {0, 1, 2}, {0, 2, 1, 3}, 0b111);
    Result r = Leave(c, 2);  // pad 3 answers player 2's slot
    EXPECT(r.did == Did::Leave && r.seat == 1 && r.screen == 1);
    r = Leave(c, 0);
    EXPECT(r.did == Did::Refused && r.seat == 0 && r.screen == 0);
    EXPECT(Leave(c, 3).did == Did::Nothing);  // the spare slot
    Claim two = Open(2, {0, 1, -1}, kIdentity, 0b11);
    EXPECT(Leave(two, 2).did == Did::Nothing);  // slot 3 holds no player
}

/// The leave's hold fires once, after a second, and again only after a let-go; B itself is never held back.
void TheLeaveHold()
{
    Held h{};
    EXPECT(!Hold(h, true, 0, kLeaveHoldMs));
    EXPECT(!Hold(h, true, kLeaveHoldMs - 1, kLeaveHoldMs));
    EXPECT(Hold(h, true, kLeaveHoldMs, kLeaveHoldMs));
    EXPECT(!Hold(h, true, kLeaveHoldMs + 500, kLeaveHoldMs));
    EXPECT(!Hold(h, false, kLeaveHoldMs + 600, kLeaveHoldMs));
    EXPECT(!Hold(h, true, 5000, kLeaveHoldMs));
    EXPECT(Hold(h, true, 5000 + kLeaveHoldMs, kLeaveHoldMs));
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

/// Any run of presses keeps every player on its screen and the table a permutation.
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
            EXPECT(c.seatOn == c.openSeatOn);
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
    TheMiddleScreenTakesItsPlayer();
    TakingIsExclusive();
    PlayerOneDoesNotStep();
    KeepBackAndCancel();
    WhenItEnds();
    EverySwapKeepsTheWhole();
    TheOpener();
    TheLetGo();
    JoiningFromTheClaim();
    TheRebase();
    OpenedByAJoin();
    Leaving();
    TheLeaveHold();
    StartOnlyForPlayers();
    if (g_failures != 0)
    {
        std::printf("pad_claim_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("pad_claim_rules: all passed\n");
    return 0;
}
