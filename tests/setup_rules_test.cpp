// Host-side tests for the setup app's rules, run without the game. The desktop: the user's row of 2560 | 3440 | 2560 with a
// fourth monitor above it, and the same with a monitor scaled by Windows.

#include "platform/settings.h"
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

Look LookOf(std::span<const Monitor> monitors, std::vector<std::size_t> picks, std::size_t game = 0)
{
    return LookAt(monitors, picks, game);
}

void NonePicked()
{
    const Look look = LookOf(kDesktop, {});
    EXPECT(look.ok && !look.picked);
    EXPECT((look.screens == std::vector<std::size_t>{2, 0, 1}));  // left to right, the monitor above left alone
}

void PickedPair()
{
    Look look = LookOf(kDesktop, {0, 1});
    EXPECT(look.ok && look.picked);
    EXPECT((look.screens == std::vector<std::size_t>{0, 1}));
    EXPECT(look.text.find(L"move") == std::wstring::npos);  // already a row: nothing to say about moving

    // Any monitors, in the order clicked: a gap between them, one above the other, the order reversed.
    look = LookOf(kDesktop, {1, 2});
    EXPECT(look.ok && (look.screens == std::vector<std::size_t>{1, 2}));
    EXPECT(look.text.find(L"go back afterwards") != std::wstring::npos);
    EXPECT(LookOf(kDesktop, {0, 3}).ok);
    look = LookOf(kDesktop, {1, 0, 2});
    EXPECT(look.ok && (look.screens == std::vector<std::size_t>{1, 0, 2}));
}

void PickedBadly()
{
    EXPECT(!LookOf(kDesktop, {0}).ok);            // one monitor
    EXPECT(!LookOf(kDesktop, {0, 1, 2, 3}).ok);   // more than a row takes
}

/// A pick as saved must read back through the settings file's own reader, which lower-cases and drops spaces and commas, and
/// still find its monitor when its path has changed: a virtual monitor comes back with a new one, under its old name.
void PickSurvivesTheSettingsFile()
{
    namespace row = lss::views::monitor_row;
    namespace settings = lss::platform::settings;
    const std::wstring before = L"\\\\?\\display#smkd1ce#1&15ecd195&1&uid256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    const std::wstring after  = L"\\\\?\\display#smkd1ce#1&15ecd195&1&uid257#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    const std::wstring dell   = L"\\\\?\\display#dela199#5&21434701&1&uid4355#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    const std::vector<std::wstring> words{row::PickWord(dell, row::IdentityKey(dell, L"DELL S2721D")),
                                          row::PickWord(before, row::IdentityKey(before, L"Steam Deck, Living Room   "))};
    const std::wstring saved = row::NamesWord(words);
    const std::wstring read  = settings::Judge(settings::kMonitors, saved).word;
    EXPECT(read == saved);

    const std::vector<std::wstring> ids{after, dell};
    const std::vector<std::wstring> keys{row::IdentityKey(after, L"Steam Deck, Living Room   "), row::IdentityKey(dell, L"DELL S2721D")};
    const row::Picks picks = row::FindPicks(ids, keys, read);
    EXPECT(picks.named == 2 && (picks.found == std::vector<std::size_t>{1, 0}));
}

/// Any monitors make a row when picked: other heights, Windows scaling. Only a view too wide for a clean picture is warned about.
void AnyMonitors()
{
    const std::vector<Monitor> mixed{At(0, 0, 3440), At(3440, 0, 1280, 720), At(-2560, 0, 2560, 1440, 144), At(4720, 0, 5120, 1440)};
    Look look = LookOf(mixed, {0, 1});
    EXPECT(look.ok && (look.screens == std::vector<std::size_t>{0, 1}));
    EXPECT(look.text.find(L"wrong colours") == std::wstring::npos);
    EXPECT(LookOf(mixed, {2, 0}).ok);
    look = LookOf(mixed, {0, 3});
    EXPECT(look.ok && look.text.find(L"wrong colours") != std::wstring::npos);
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
    PickSurvivesTheSettingsFile();
    AnyMonitors();
    PlayersOnScreens();
    PictureKeepsTheArrangement();
    std::printf(g_failures == 0 ? "setup rules: all passed\n" : "setup rules: %d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
