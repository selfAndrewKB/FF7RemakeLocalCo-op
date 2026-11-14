// Host-side tests for the low-health red edge's rules (render/low_health_rules.h). Run without the game.

#include "render/low_health_rules.h"

#include <array>
#include <cstdio>

namespace {

using namespace lss::render::low_health_rules;
namespace columns = lss::views::columns;

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

constexpr std::uint8_t kCloud = 0, kBarret = 1, kTifa = 2, kAerith = 3;
constexpr Held         kPlayerTwoOnTifa{kNoType, kTifa, kNoType, kNoType};

/// A seat's controller counts its own member only.
void SeatCountsItsOwn()
{
    const Held held{kNoType, kTifa, kAerith, kNoType};
    EXPECT(Counts(1, kTifa, held));
    EXPECT(!Counts(1, kCloud, held));
    EXPECT(!Counts(1, kAerith, held));
    EXPECT(!Counts(1, kBarret, held));
    EXPECT(Counts(2, kAerith, held));
    EXPECT(!Counts(2, kTifa, held));
}

/// A seat that holds no member counts nothing, not even a member of type NONE.
void EmptySeatCountsNothing()
{
    const Held held{kNoType, kNoType, kNoType, kNoType};
    EXPECT(!Counts(1, kTifa, held));
    EXPECT(!Counts(1, kNoType, held));
    EXPECT(!Counts(3, kCloud, held));
}

/// Player 1 counts its own member and every member no seat holds.
void PlayerOneCountsTheRest()
{
    EXPECT(Counts(0, kCloud, kPlayerTwoOnTifa));
    EXPECT(Counts(0, kBarret, kPlayerTwoOnTifa));  // an AI member
    EXPECT(!Counts(0, kTifa, kPlayerTwoOnTifa));   // player 2's
    const Held none{kNoType, kNoType, kNoType, kNoType};
    EXPECT(Counts(0, kTifa, none));  // alone: the game's whole party
}

/// A player out of range counts nothing.
void OutOfRange()
{
    EXPECT(!Counts(kPlayers, kTifa, kPlayerTwoOnTifa));
}

void Targets()
{
    EXPECT(Target(true, true, 0.8f) == 0.8f);
    EXPECT(Target(true, false, 0.8f) == 0.0f);  // an event hides it
    EXPECT(Target(false, true, 0.8f) == 0.0f);
}

/// A view never shows more than the game's value: the game's gates (a menu's fade-out) reach every view.
void TheCap()
{
    EXPECT(ViewIntensity(0.8f, 0.8f) == 0.8f);
    EXPECT(ViewIntensity(0.3f, 0.8f) == 0.3f);  // fading in on its own
    EXPECT(ViewIntensity(0.8f, 0.2f) == 0.2f);  // a menu fades the game's out
    EXPECT(ViewIntensity(0.0f, 0.8f) == 0.0f);
}

/// Each column's player; a mirror shows player 1's picture.
void Views()
{
    const std::array<int, 3> places{0, 2, columns::kMirror};
    const columns::Map map = columns::MapOf(places);
    EXPECT(ViewPlayer(0, map) == 0);
    EXPECT(ViewPlayer(1, map) == 2);
    EXPECT(ViewPlayer(2, map) == 0);
    EXPECT(ViewPlayer(3, map) == -1);  // not listed
    EXPECT(ViewPlayer(-1, map) == -1);
    EXPECT(ViewPlayer(-2, map) == -1);
    const std::array<int, 2> unknown{0, columns::kUnknown};
    EXPECT(ViewPlayer(1, columns::MapOf(unknown)) == -1);
}

}  // namespace

int main()
{
    SeatCountsItsOwn();
    EmptySeatCountsNothing();
    PlayerOneCountsTheRest();
    OutOfRange();
    Targets();
    TheCap();
    Views();
    if (g_failures != 0)
    {
        std::printf("low_health_rules: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("low_health_rules: all passed\n");
    return 0;
}
