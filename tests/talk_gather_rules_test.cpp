// Host-side tests for the gather's spot rule (world/talk_gather_rules.h): the three directions and the facing, with
// and without the NPC's place. Run without the game.

#include "world/talk_gather_rules.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "talk_gather_rules: all passed\n" : "talk_gather_rules: %d failure(s)\n", g_failures);
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

using namespace lss::world::talk_gather_rules;

bool Near(const Vec3& a, float x, float y)
{
    return std::fabs(a.x - x) < 1e-4f && std::fabs(a.y - y) < 1e-4f && a.z == 0.0f;
}

/// Without the NPC's place: toward player 1's character, then its left and right; facing the presser.
void WithoutNpc()
{
    const Vec3   presser{0, 0, 0}, cloud{1000, 0, 0};
    const Beside b = BesideSpot(presser, cloud, nullptr);
    EXPECT(!b.withNpc);
    EXPECT(Near(b.tries[0], 1, 0));
    EXPECT(Near(b.tries[1], 0, 1));
    EXPECT(Near(b.tries[2], 0, -1));
    EXPECT(b.face.x == 0 && b.face.y == 0);
}

/// With the NPC north of the presser and player 1's character east: the east side of the line first, then the west,
/// then behind (south); facing the NPC.
void WithNpc()
{
    const Vec3   presser{0, 0, 0}, cloud{1000, 0, 0}, npc{0, 200, 0};
    const Beside b = BesideSpot(presser, cloud, &npc);
    EXPECT(b.withNpc);
    EXPECT(Near(b.tries[0], 1, 0));   // the side facing player 1's character
    EXPECT(Near(b.tries[1], -1, 0));  // the far side
    EXPECT(Near(b.tries[2], 0, -1));  // behind the presser, away from the NPC
    EXPECT(b.face.y == 200);
    // Player 1's character west instead: the sides swap.
    const Vec3   west{-1000, 0, 0};
    const Beside w = BesideSpot(presser, west, &npc);
    EXPECT(Near(w.tries[0], -1, 0) && Near(w.tries[1], 1, 0));
}

/// An NPC place too near (another record of the presser itself) or too far (another actor of the name) does not count.
void NpcOutOfRange()
{
    const Vec3 presser{0, 0, 0}, cloud{1000, 0, 0}, same{0.5f, 0, 0}, oneCm{1.0f, 0, 0}, tooFar{0, kFarCm + 1, 0},
        edge{0, kFarCm, 0};
    EXPECT(!BesideSpot(presser, cloud, &same).withNpc);
    EXPECT(!BesideSpot(presser, cloud, &oneCm).withNpc);  // strictly more than 1 cm
    EXPECT(!BesideSpot(presser, cloud, &tooFar).withNpc);
    EXPECT(BesideSpot(presser, cloud, &edge).withNpc);
    // Player 1's character on the presser's line to the NPC (the dot product 0): the left side first.
    const Vec3   npc{0, 200, 0}, onLine{0, 1000, 0};
    const Beside b = BesideSpot(presser, onLine, &npc);
    EXPECT(b.withNpc && Near(b.tries[0], -1, 0));
}

/// Coinciding points give a direction rather than NaN.
void Coincident()
{
    const Vec3   p{5, 5, 0};
    const Beside b = BesideSpot(p, p, nullptr);
    EXPECT(Near(b.tries[0], 1, 0));
}

void YawAndNames()
{
    EXPECT(std::fabs(FacingYaw(Vec3{0, 0, 0}, Vec3{0, 10, 0}) - 90.0f) < 1e-3f);
    EXPECT(std::fabs(FacingYaw(Vec3{0, 0, 0}, Vec3{-10, 0, 0}) - 180.0f) < 1e-3f);
    EXPECT(std::wcscmp(SpotName(false, 0), L"toward where it stood") == 0);
    EXPECT(std::wcscmp(SpotName(true, 2), L"behind") == 0);
    EXPECT(std::wcscmp(SpotName(true, 3), L"nowhere") == 0);
}

/// Player 1's own talk: a planner event on the pressed object's channel, soon after the press.
void PlayerOnesTalk()
{
    const std::uint64_t npc = 0x1234, other = 0x9999, at = 50000;
    EXPECT(PlayerOneTalk(npc, at, npc, at, true));
    EXPECT(PlayerOneTalk(npc, at, npc, at + kPressFreshMs, true));
    EXPECT(!PlayerOneTalk(npc, at, npc, at + kPressFreshMs + 1, true));  // too long after the press
    EXPECT(!PlayerOneTalk(npc, at, other, at + 10, true));               // another object's channel took the lock
    EXPECT(!PlayerOneTalk(npc, at, npc, at + 10, false));                // a cutscene act, not a talk's
    // A talk's acts: the two planner events and the shop a shopkeeper's talk opens; a cutscene act and an unread row not.
    EXPECT(TalkAct(1) && TalkAct(16) && TalkAct(43));
    EXPECT(!TalkAct(0) && !TalkAct(-1) && !TalkAct(56) && !TalkAct(4));
    EXPECT(!PlayerOneTalk(0, 0, 0, 10, true));                           // no press, no object
    EXPECT(!PlayerOneTalk(0, 0, npc, 10, true));                         // no press
    EXPECT(!PlayerOneTalk(npc, at, npc, at - 1, true));                  // a clock that ran back
}

}  // namespace

int main()
{
    PlayerOnesTalk();
    WithoutNpc();
    WithNpc();
    NpcOutOfRange();
    Coincident();
    YawAndNames();
    return lss::test::Report();
}
