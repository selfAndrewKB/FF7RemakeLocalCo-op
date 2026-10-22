// Host-side tests for the seats header (players/seats.h): the seat numbering, the names and the bounds the session
// and the per-seat modules index by. Run without the game.

#include "players/seats.h"

#include <cstdio>
#include <cwchar>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "seats: all passed\n" : "seats: %d failure(s)\n", g_failures);
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

using namespace lss::players;

bool Same(const wchar_t* a, const wchar_t* b)
{
    return std::wcscmp(a, b) == 0;
}

void Numbering()
{
    static_assert(kSeatsBuilt >= 1, "at least player 2");
    static_assert(kSeatsBuilt < scopes::kMaxPlayers, "the seats fit the scopes' players");
    EXPECT(!SeatBuilt(0));           // player 1 is the game's, never a seat
    EXPECT(SeatBuilt(1));            // player 2
    EXPECT(!SeatBuilt(kSeatsBuilt + 1));
    EXPECT(!SeatBuilt(-1));
    EXPECT(!SeatBuilt(scopes::kMaxPlayers));
}

void Names()
{
    EXPECT(Same(SeatName(0), L"player 1"));
    EXPECT(Same(SeatName(1), L"player 2"));
    EXPECT(Same(SeatName(2), L"player 3"));
    EXPECT(Same(SeatName(3), L"player 4"));
    EXPECT(Same(SeatName(-1), L"a player"));
    EXPECT(Same(SeatName(scopes::kMaxPlayers), L"a player"));
    static_assert(SeatName(1)[7] == L'2', "constexpr name");
}

}  // namespace

int main()
{
    Numbering();
    Names();
    return lss::test::Report();
}
