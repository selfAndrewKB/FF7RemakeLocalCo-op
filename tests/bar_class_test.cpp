// Host-side tests for the enemy bar's class in a further column (hud/bar_class.h). Run without the game.

#include "hud/bar_class.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "bar_class: all passed\n" : "bar_class: %d failure(s)\n", g_failures);
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

using namespace lss::hud::bar_class;

int a = 0, b = 0, c = 0, d = 0;  // enemies (addresses only)
const void* const A = &a;
const void* const B = &b;
const void* const C = &c;
const void* const D = &d;

void DifferentTargets()
{
    // Player 1 targets A, player 2 targets B. The tick placed A targeted (a), B untargeted.
    const Keys one{A, nullptr}, two{B, nullptr};
    const Verdict onA = Classify(A, one, two, true, false);
    EXPECT(!onA.own && !onA.main && !onA.caseC && !onA.forced);
    EXPECT(!onA.targeted);  // player 1's target: untargeted in player 2's column
    const Verdict onB = Classify(B, one, two, false, false);
    EXPECT(onB.own && onB.targeted && !onB.forced);  // player 2's target: targeted there
    const Verdict onC = Classify(C, one, two, false, false);
    EXPECT(!onC.targeted);
}

void SameTarget()
{
    const Keys one{A, nullptr}, two{A, nullptr};
    const Verdict v = Classify(A, one, two, true, false);
    EXPECT(v.own && v.targeted && !v.forced);
}

void MainTarget()
{
    // Player 2 locks a part B whose main target is A: A's bar is targeted (b) in player 2's column.
    const Keys one{C, nullptr}, two{B, A};
    EXPECT(Classify(A, one, two, false, false).main);
    EXPECT(Classify(A, one, two, false, false).targeted);
    EXPECT(Classify(B, one, two, false, false).own);
    // Player 1's main target D placed targeted by the tick: neither (c) nor (d), untargeted for player 2.
    const Verdict onD = Classify(D, Keys{C, D}, two, true, false);
    EXPECT(!onD.forced && !onD.targeted);
}

void CaseC()
{
    // The tick placed C targeted with the leader clamp skipped: the always-show case, targeted in every column.
    const Keys one{A, nullptr}, two{B, nullptr};
    const Verdict v = Classify(C, one, two, true, true);
    EXPECT(v.caseC && v.targeted && !v.forced);
}

void Forced()
{
    // The tick placed D targeted, D is neither player's key and not case (c): the bar's own state (recently hit).
    const Keys one{A, nullptr}, two{B, nullptr};
    const Verdict v = Classify(D, one, two, true, false);
    EXPECT(v.forced && v.targeted && !v.caseC);
    // An untargeted kind is never forced.
    EXPECT(!Classify(D, one, two, false, false).forced);
}

void NoKeys()
{
    // Nobody targets anything: only the tick's forced bars are targeted; a null source never matches a null key.
    const Keys none{};
    EXPECT(!Classify(A, none, none, false, false).targeted);
    EXPECT(Classify(A, none, none, true, false).forced);
    EXPECT(!Classify(nullptr, none, none, false, false).own);
}

void PlayerOneCheck()
{
    const Keys one{A, nullptr};
    EXPECT(PlayerOneAgrees(A, one, true, false));
    EXPECT(PlayerOneAgrees(B, one, false, false));
    EXPECT(PlayerOneAgrees(B, one, true, true));   // case (c)
    EXPECT(PlayerOneAgrees(B, one, true, false));  // forced: the rule takes the tick's word
    EXPECT(!PlayerOneAgrees(A, one, false, false));  // the key's bar placed untargeted: a disagreement
}

}  // namespace

int main()
{
    static_assert(Classify(nullptr, Keys{}, Keys{}, false, false).targeted == false, "constexpr");
    DifferentTargets();
    SameTarget();
    MainTarget();
    CaseC();
    Forced();
    NoKeys();
    PlayerOneCheck();
    return lss::test::Report();
}
