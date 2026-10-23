// Host-side tests for a seat's state word (players/seat_state.h). Run without the game.

#include "players/seat_state.h"

#include <cstdio>
#include <cwchar>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "seat_state: all passed\n" : "seat_state: %d failure(s)\n", g_failures);
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

using lss::players::SeatFlags;
using lss::players::SeatState;
using lss::players::StateName;
using lss::players::StateOf;

bool Same(const wchar_t* a, const wchar_t* b)
{
    return std::wcscmp(a, b) == 0;
}

/// Out of the game: out, awaiting a member, or rejoining after a world change (the rejoin first).
void OutOfTheGame()
{
    EXPECT(StateOf({}) == SeatState::Out);
    EXPECT(StateOf({.awaiting = true}) == SeatState::Awaiting);
    EXPECT(StateOf({.rejoinPending = true}) == SeatState::Rejoining);
    EXPECT(StateOf({.awaiting = true, .rejoinPending = true}) == SeatState::Rejoining);
    // Stale member flags with the player out never make a word of their own.
    EXPECT(StateOf({.seated = true, .handedToAi = true}) == SeatState::Out);
}

/// In the game: in (no member yet), playing, or handed while the member is with its AI.
void InTheGame()
{
    EXPECT(StateOf({.in = true}) == SeatState::In);
    EXPECT(StateOf({.in = true, .seated = true}) == SeatState::Playing);
    EXPECT(StateOf({.in = true, .seated = true, .handedToAi = true}) == SeatState::Handed);
    // A hand without a member is not a hand: the member is what is handed.
    EXPECT(StateOf({.in = true, .handedToAi = true}) == SeatState::In);
    // In the game, the out-of-game flags change nothing.
    EXPECT(StateOf({.in = true, .seated = true, .awaiting = true, .rejoinPending = true}) == SeatState::Playing);
}

/// The reading's words, as the console scripts wait for them.
void Words()
{
    EXPECT(Same(StateName(SeatState::Out), L"out"));
    EXPECT(Same(StateName(SeatState::Awaiting), L"awaiting"));
    EXPECT(Same(StateName(SeatState::Rejoining), L"rejoining"));
    EXPECT(Same(StateName(SeatState::In), L"in"));
    EXPECT(Same(StateName(SeatState::Playing), L"playing"));
    EXPECT(Same(StateName(SeatState::Handed), L"handed"));
    static_assert(StateOf({.in = true, .seated = true}) == SeatState::Playing, "constexpr");
}

}  // namespace

int main()
{
    OutOfTheGame();
    InTheGame();
    Words();
    return lss::test::Report();
}
