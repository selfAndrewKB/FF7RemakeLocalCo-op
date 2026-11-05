// Host-side tests for where the title menu's Co-op row goes, run without the game: both start menus' lists as the game
// builds them, a list rebuilt or not, a list without Options.

#include "menu/title_row_rules.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using lss::menu::title_row_rules::PlaceBelow;

int g_failures = 0;

#define EXPECT(condition)                                                \
    do                                                                   \
    {                                                                    \
        if (!(condition))                                                \
        {                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition); \
            ++g_failures;                                                \
        }                                                                \
    } while (false)

constexpr std::int32_t kOptions = 3, kCoop = 6;

std::optional<std::size_t> Place(std::vector<std::int32_t> kinds)
{
    return PlaceBelow(kinds, kOptions, kCoop);
}

}  // namespace

int main()
{
    EXPECT(Place({0, 1, 2, 3, 7, 4}) == 4);       // the main game's list, Members below Options
    EXPECT(Place({0, 1, 2, 3, 4}) == 4);          // the Intermission part's
    EXPECT(!Place({0, 1, 2, 3, 6, 7, 4}));        // already in: never twice
    EXPECT(!Place({0, 1, 2, 4}));                 // no Options row
    EXPECT(!Place({}));
    std::printf(g_failures == 0 ? "title row rules: all passed\n" : "title row rules: %d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
