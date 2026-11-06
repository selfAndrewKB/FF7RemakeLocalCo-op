// Host-side tests for area_levels_rules (world/area_levels_rules.h): the one level request every player's area shares.
// Run without the game.
//
// The case, as played: player 2 in the apartment block (its area lists 165-SlumAptB shown), player 1 walks
// to Area040, whose list drops it; the game unloaded player 2's floor. Names here are made-up numbers; only equality
// matters.

#include "world/area_levels_rules.h"

#include <cstdio>
#include <vector>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "area_levels_rules: all passed\n" : "area_levels_rules: %d failure(s)\n", g_failures);
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

using namespace lss::world::area_levels_rules;

constexpr std::uint64_t kArea040    = 40;
constexpr std::uint64_t kArea040Col = 41;
constexpr std::uint64_t kAptB       = 165;
constexpr std::uint64_t kAptBCol    = 166;
constexpr std::uint64_t kRdlA       = 85;
constexpr std::uint64_t kFarTwin    = 900;

struct Lists
{
    std::vector<std::uint64_t> names;
    std::vector<std::uint8_t>  hidden;
    Area                       View() const { return {names, hidden}; }
};

void TestAloneIsPlayerOnesList()
{
    const Lists   one{{kArea040, kArea040Col, kRdlA}, {0, 0, 1}};
    const Request request = Merge(one.View(), {});
    EXPECT(request.count == 3);
    EXPECT(request.names[0] == kArea040 && request.names[2] == kRdlA);
    EXPECT(request.hidden[2] == 1);
    EXPECT(request.added == 0 && request.changed == 0 && request.cut == 0);
}

void TestTheFurtherPlayersFloorStays()
{
    const Lists   one{{kArea040, kArea040Col}, {0, 0}};
    const Lists   two{{kAptB, kAptBCol, kArea040}, {0, 0, 0}};
    const Area    others[] = {two.View()};
    const Request request  = Merge(one.View(), others);
    EXPECT(request.count == 4);
    EXPECT(request.names[0] == kArea040 && request.names[1] == kArea040Col);  // player 1's first, in the game's order
    EXPECT(request.names[2] == kAptB && request.names[3] == kAptBCol);
    EXPECT(request.added == 2 && request.changed == 0);
}

void TestEachLevelOnce()
{
    const Lists   one{{kArea040, kArea040, kRdlA}, {1, 0, 1}};  // a copy in player 1's own list too
    const Lists   two{{kRdlA, kAptB, kAptB}, {1, 0, 0}};
    const Lists   three{{kAptB, kArea040}, {0, 0}};
    const Area    others[] = {two.View(), three.View()};
    const Request request  = Merge(one.View(), others);
    EXPECT(request.count == 3);
    EXPECT(request.hidden[0] == 0);  // player 1's two copies: shown wins
    EXPECT(request.added == 1);
}

void TestShownWinsThenCollision()
{
    EXPECT(Combine(1, 0) == 0);
    EXPECT(Combine(0, 1) == 0);
    EXPECT(Combine(2, 0) == 0);
    EXPECT(Combine(1, 2) == 2);
    EXPECT(Combine(2, 1) == 2);
    EXPECT(Combine(1, 1) == 1);
    // The near/far twin: player 1 lists it hidden, player 2 shows it.
    const Lists   one{{kArea040, kFarTwin}, {0, 1}};
    const Lists   two{{kFarTwin}, {0}};
    const Area    others[] = {two.View()};
    const Request request  = Merge(one.View(), others);
    EXPECT(request.count == 2 && request.hidden[1] == 0);
    EXPECT(request.changed == 1 && request.added == 0);
    EXPECT(ShownAgainst(request, one.View()) == 1);  // player 1's view shows what player 1's area hides
    EXPECT(ShownAgainst(request, two.View()) == 0);
}

void TestMissingBytesReadShown()
{
    const Lists   one{{kArea040, kRdlA}, {}};
    const Request request = Merge(one.View(), {});
    EXPECT(request.count == 2 && request.hidden[0] == 0 && request.hidden[1] == 0);
}

void TestNeverPastTheRequestsRoom()
{
    Lists one, two;
    for (std::uint64_t i = 0; i < 250; ++i)
    {
        one.names.push_back(1000 + i);
        one.hidden.push_back(0);
    }
    // Player 2: four hidden levels listed first, then four shown, then two hidden with collision.
    for (std::uint64_t i = 0; i < 4; ++i)
    {
        two.names.push_back(5000 + i);
        two.hidden.push_back(1);
    }
    for (std::uint64_t i = 0; i < 4; ++i)
    {
        two.names.push_back(6000 + i);
        two.hidden.push_back(0);
    }
    two.names.push_back(7000);
    two.hidden.push_back(2);
    two.names.push_back(7001);
    two.hidden.push_back(2);
    const Area    others[] = {two.View()};
    const Request request  = Merge(one.View(), others);
    EXPECT(request.count == kMaxLevels);
    EXPECT(request.cut == 4);  // the four hidden ones: shown first, then hidden with collision
    EXPECT(Find(request, 6000) >= 0 && Find(request, 6003) >= 0);
    EXPECT(Find(request, 7000) >= 0 && Find(request, 7001) >= 0);
    EXPECT(Find(request, 5000) < 0);
    EXPECT(Find(request, 1000) == 0 && Find(request, 1249) == 249);  // player 1's all kept
}

void TestPlayerOnesOwnOverflowIsCut()
{
    Lists one;
    for (std::uint64_t i = 0; i < 260; ++i)
    {
        one.names.push_back(1000 + i);
    }
    const Request request = Merge(one.View(), {});
    EXPECT(request.count == kMaxLevels && request.cut == 4);
}

void TestEquality()
{
    const Lists one{{kArea040, kRdlA}, {0, 1}};
    const Lists other{{kArea040, kRdlA}, {0, 0}};
    EXPECT(Merge(one.View(), {}) == Merge(one.View(), {}));
    EXPECT(!(Merge(one.View(), {}) == Merge(other.View(), {})));  // a byte differs
}

}  // namespace

int main()
{
    TestAloneIsPlayerOnesList();
    TestTheFurtherPlayersFloorStays();
    TestEachLevelOnce();
    TestShownWinsThenCollision();
    TestMissingBytesReadShown();
    TestNeverPastTheRequestsRoom();
    TestPlayerOnesOwnOverflowIsCut();
    TestEquality();
    return lss::test::Report();
}
