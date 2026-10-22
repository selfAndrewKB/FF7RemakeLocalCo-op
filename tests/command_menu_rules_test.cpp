// Host-side tests for the command menus' shared rules (battle/command_menu_rules.h): the world-slow write that
// every controller's tick makes, player 1's first and then each seat's. Run without the game.

#include "battle/command_menu_rules.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "command_menu_rules: all passed\n" : "command_menu_rules: %d failure(s)\n", g_failures);
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

using lss::battle::command_menu_rules::SlowWrites;

/// Any menu slows the world: a seat's write takes the least of the frame's values so far.
void LeastOfTheFrame()
{
    SlowWrites slow;
    slow.BeginFrame();
    EXPECT(slow.Write(0, 1.0f) == 1.0f);   // player 1's menu closed: the game's write as is
    EXPECT(slow.Write(1, 0.05f) == 0.05f); // player 2's menu open: its write slows
    EXPECT(slow.Write(2, 1.0f) == 0.05f);  // player 3's menu closed: keeps player 2's slow
    EXPECT(slow.Alone() == 0);

    slow.BeginFrame();
    EXPECT(slow.Write(0, 0.05f) == 0.05f); // player 1's menu open
    EXPECT(slow.Write(1, 1.0f) == 0.05f);  // player 2's closed: the world stays slowed
    EXPECT(slow.Write(2, 0.02f) == 0.02f); // player 3's slower still (hard mode's value)
    EXPECT(slow.Alone() == 0);
}

/// Player 1's write starts a frame's least even without BeginFrame (the game's tick writes every frame).
void PlayerOneStartsTheFrame()
{
    SlowWrites slow;
    slow.BeginFrame();
    EXPECT(slow.Write(0, 1.0f) == 1.0f);
    EXPECT(slow.Write(1, 0.05f) == 0.05f);
    EXPECT(slow.Write(0, 1.0f) == 1.0f);  // the next frame's game write, no BeginFrame between: the slow is gone
    EXPECT(slow.Write(1, 1.0f) == 1.0f);
    EXPECT(slow.Alone() == 0);
}

/// A seat's write with no write of player 1's in the frame goes as asked, and is counted.
void AloneWrites()
{
    SlowWrites slow;
    slow.BeginFrame();
    EXPECT(slow.Write(1, 0.05f) == 0.05f);
    EXPECT(slow.Alone() == 1);
    EXPECT(slow.Write(2, 1.0f) == 0.05f);  // player 3's joins player 2's frame: the least, not alone
    EXPECT(slow.Alone() == 1);
    slow.BeginFrame();
    EXPECT(slow.Alone() == 1);            // the count is for the game's life, not the frame
    EXPECT(slow.Write(1, 1.0f) == 1.0f);   // as asked again
    EXPECT(slow.Alone() == 2);
}

/// Player 1's write replaces whatever came before it in the frame: the order the game ticks in is the rule's.
void PlayerOneReplaces()
{
    SlowWrites slow;
    slow.BeginFrame();
    EXPECT(slow.Write(1, 0.05f) == 0.05f);  // a seat first (never the game's order)
    EXPECT(slow.Write(0, 1.0f) == 1.0f);    // player 1's write starts the frame's least over it
    EXPECT(slow.Write(1, 1.0f) == 1.0f);
    EXPECT(slow.Alone() == 1);

    slow.BeginFrame();
    EXPECT(slow.Write(0, 1.0f) == 1.0f);
    EXPECT(slow.Write(1, 0.05f) == 0.05f);
    EXPECT(slow.Write(0, 1.0f) == 1.0f);    // player 1 writing twice in a frame: the second starts over
    EXPECT(slow.Write(2, 0.5f) == 0.5f);
    EXPECT(slow.Alone() == 1);
}

/// Equal values stay: a seat asking what the frame's least already is changes nothing.
void EqualValues()
{
    SlowWrites slow;
    slow.BeginFrame();
    EXPECT(slow.Write(0, 0.05f) == 0.05f);
    EXPECT(slow.Write(1, 0.05f) == 0.05f);
    EXPECT(slow.Write(2, 0.05f) == 0.05f);
    EXPECT(slow.Alone() == 0);
}

}  // namespace

int main()
{
    LeastOfTheFrame();
    PlayerOneStartsTheFrame();
    AloneWrites();
    PlayerOneReplaces();
    EqualValues();
    return lss::test::Report();
}
