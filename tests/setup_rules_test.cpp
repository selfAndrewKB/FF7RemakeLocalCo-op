// Host-side tests for the setup app's rules, run without the game. The desktop: the user's row of 2560 | 3440 | 2560 with a
// fourth monitor above it, and the same with a monitor scaled by Windows.

#include "setup/setup_rules.h"

#include <cstdio>
#include <vector>

namespace {

using namespace lss::setup::rules;
using lss::views::monitor_row::Monitor;
using lss::views::monitor_row::Rect;

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

Monitor At(std::int32_t left, std::int32_t top, std::int32_t width = 2560, std::int32_t height = 1440, std::uint32_t dpi = 96)
{
    return {{left, top, left + width, top + height}, dpi};
}

// Primary first, as Windows lists it.
const std::vector<Monitor> kDesktop{At(0, 0, 3440), At(3440, 0), At(-2560, 0), At(2585, -1440)};

Look LookOf(std::span<const Monitor> monitors, std::vector<char> ticked, std::size_t game = 0)
{
    return LookAt(monitors, ticked, game);
}

void NonePicked()
{
    const Look look = LookOf(kDesktop, {0, 0, 0, 0});
    EXPECT(look.ok && !look.picked);
    EXPECT((look.screens == std::vector<std::size_t>{2, 0, 1}));  // left to right, the monitor above left alone
}

void PickedPair()
{
    const Look look = LookOf(kDesktop, {1, 1, 0, 0});
    EXPECT(look.ok && look.picked);
    EXPECT((look.screens == std::vector<std::size_t>{0, 1}));
}

void PickedBadly()
{
    EXPECT(!LookOf(kDesktop, {1, 0, 0, 0}).ok);  // one monitor
    EXPECT(!LookOf(kDesktop, {0, 1, 1, 0}).ok);  // a gap between them
    EXPECT(!LookOf(kDesktop, {1, 0, 0, 1}).ok);  // one above the other
    EXPECT(!LookOf(kDesktop, {1, 1, 1, 1}).ok);  // more than a row takes
}

void ScaledMonitorSaysWhy()
{
    EXPECT(NeverJoins(At(0, 0)).empty());
    EXPECT(NeverJoins(At(0, 0, 3840, 2160, 168)) == L"Scaled to 175 %: needs 100 % in Windows");
}

void PlayersOnScreens()
{
    EXPECT(PlayerOnScreen(L"123", 3, 0) == 1 && PlayerOnScreen(L"123", 3, 2) == 3);
    EXPECT(PlayerOnScreen(L"213", 3, 0) == 2 && PlayerOnScreen(L"213", 3, 1) == 1);
    EXPECT(PlayerOnScreen(L"312", 2, 0) == 1 && PlayerOnScreen(L"312", 2, 1) == 2);  // no third screen: player 3 skipped
    EXPECT(!PlayerOnScreen(L"123", 2, 2));
}

void PictureKeepsTheArrangement()
{
    const auto placed = Fit(kDesktop, 800, 400, 10);
    EXPECT(placed.size() == 4);
    EXPECT(placed[2].right <= placed[0].left + 1 && placed[0].right <= placed[1].left + 1);  // left, middle, right
    EXPECT(placed[3].bottom <= placed[0].top + 1);                                            // above
    for (const Rect& rect : placed)
    {
        EXPECT(rect.left >= 10 && rect.top >= 10 && rect.right <= 790 && rect.bottom <= 390);
    }
    EXPECT(Fit({}, 800, 400, 10).empty());
}

}  // namespace

int main()
{
    NonePicked();
    PickedPair();
    PickedBadly();
    ScaledMonitorSaysWhy();
    PlayersOnScreens();
    PictureKeepsTheArrangement();
    std::printf(g_failures == 0 ? "setup rules: all passed\n" : "setup rules: %d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
