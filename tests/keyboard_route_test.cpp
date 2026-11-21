// Host-side tests for players::keyboard_route_rules, run without the game: each key's events go where its press went.

#include "players/keyboard_route_rules.h"

#include <array>
#include <cstdio>

namespace {

using lss::players::keyboard_route_rules::DownKey;
using lss::players::keyboard_route_rules::KeyEvent;
using lss::players::keyboard_route_rules::KeyId;

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

constexpr std::uint64_t kW = 0x57, kMouse = 0x99;

/// W pressed while the keyboard is player 1's, held through a join (the route now player 2's): its repeats and its
/// release stay player 1's; a press after the join is player 2's, and so are its repeats and release.
void HeldThroughAJoin()
{
    std::array<DownKey, 4> down{};
    EXPECT(KeyId(down, kW, KeyEvent::Press, 0, -1) == 0);
    EXPECT(KeyId(down, kW, KeyEvent::Other, 0, 1) == 0);    // a repeat after the join
    EXPECT(KeyId(down, kW, KeyEvent::Release, 0, 1) == 0);  // the release where the press went
    EXPECT(KeyId(down, kW, KeyEvent::Press, 0, 1) == 1);
    EXPECT(KeyId(down, kW, KeyEvent::Other, 0, -1) == 1);   // the keyboard left player 2: still player 2's until let go
    EXPECT(KeyId(down, kW, KeyEvent::Release, 0, -1) == 1);
    EXPECT(KeyId(down, kW, KeyEvent::Release, 0, 1) == 1);  // no press remembered: the route now
    EXPECT(KeyId(down, kW, KeyEvent::Release, 0, -1) == 0);
}

/// Two keys apart; a full table: a press takes the first entry; a nameless key is never remembered.
void TheTable()
{
    std::array<DownKey, 2> down{};
    EXPECT(KeyId(down, kW, KeyEvent::Press, 0, 2) == 2);
    EXPECT(KeyId(down, kMouse, KeyEvent::Press, 0, -1) == 0);
    EXPECT(KeyId(down, kW, KeyEvent::Release, 0, -1) == 2 && KeyId(down, kMouse, KeyEvent::Release, 0, 2) == 0);
    EXPECT(KeyId(down, 1, KeyEvent::Press, 0, 1) == 1 && KeyId(down, 2, KeyEvent::Press, 0, 1) == 1);
    EXPECT(KeyId(down, 3, KeyEvent::Press, 0, 2) == 2);  // full: key 1's entry taken
    EXPECT(KeyId(down, 1, KeyEvent::Release, 0, -1) == 0 && KeyId(down, 3, KeyEvent::Release, 0, -1) == 2);
    EXPECT(KeyId(down, 0, KeyEvent::Press, 0, 1) == 1 && KeyId(down, 0, KeyEvent::Release, 0, -1) == 0);
}

/// The digital stick from the direction bits: one bit alone ±1, a pair or nothing 0; the right stick from its own byte.
void TheDigitalStick()
{
    using lss::players::keyboard_route_rules::DigitalStick;
    EXPECT((DigitalStick(0, 0) == std::array<float, 4>{0, 0, 0, 0}));
    EXPECT((DigitalStick(1, 0) == std::array<float, 4>{-1, 0, 0, 0}));  // left
    EXPECT((DigitalStick(2, 0) == std::array<float, 4>{1, 0, 0, 0}));   // right
    EXPECT((DigitalStick(4, 0) == std::array<float, 4>{0, 1, 0, 0}));   // up
    EXPECT((DigitalStick(8, 0) == std::array<float, 4>{0, -1, 0, 0}));  // down
    EXPECT((DigitalStick(1 | 2 | 4, 0) == std::array<float, 4>{0, 1, 0, 0}));  // left and right cancel
    EXPECT((DigitalStick(2 | 8, 4 | 1) == std::array<float, 4>{1, -1, -1, 1}));
    EXPECT((DigitalStick(0xF0, 0xF0) == std::array<float, 4>{0, 0, 0, 0}));  // other bits: nothing
}

}  // namespace

int main()
{
    HeldThroughAJoin();
    TheTable();
    TheDigitalStick();
    if (g_failures != 0)
    {
        std::printf("keyboard_route: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("keyboard_route: all passed\n");
    return 0;
}
