// Host-side tests for views::monitor_picks, run without the game: the settings word and the monitors it finds, and the row
// those give. The row is one rule for the game and the setup app, so what the game does with a settings file is tested here.

#include "views/monitor_picks.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace lss::views::monitor_picks;
using lss::views::monitor_row::Choice;
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

/// A pick found again: the exact path first, then a monitor of the same identity, as a virtual monitor returns with a new path.
void PicksFound()
{
    const std::wstring physical = L"\\\\?\\display#gsm774b#5&21434701&1&uid4353#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    const std::wstring virtualOld = L"\\\\?\\display#smkd1ce#1&15ecd195&1&uid256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    const std::wstring virtualNew = L"\\\\?\\display#smkd1ce#1&15ecd195&1&uid257#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    EXPECT(IdentityKey(physical, L"LG ULTRAGEAR") == L"gsm774b/lg_ultragear");
    EXPECT(IdentityKey(virtualNew, L"H27T27       ") == L"smkd1ce/h27t27");
    EXPECT(IdentityKey(L"\\\\.\\display3", L"x").empty());
    EXPECT(IdentityKey(virtualNew, L"Deck|~;\u00e9") == L"smkd1ce/deck____");

    const std::wstring oldKey = IdentityKey(virtualOld, L"Steam Deck"), lgKey = IdentityKey(physical, L"LG ULTRAGEAR");
    const std::wstring word = NamesWord(std::vector<std::wstring>{PickWord(virtualOld, oldKey), PickWord(physical, lgKey)});

    // The word's order is the row's, whatever order Windows lists them in; the virtual monitor found by its identity.
    std::vector<std::wstring> ids{physical, L"\\\\?\\display#dela199#5&1&uid4355#{x}", virtualNew};
    std::vector<std::wstring> keys{lgKey, L"dela199/dell_s2721d", IdentityKey(virtualNew, L"Steam Deck")};
    auto picks = FindPicks(ids, keys, word);
    EXPECT(picks.named == 2 && picks.away == 0 && (picks.found == std::vector<std::size_t>{2, 0}));

    // Not connected: named, not found; the mod waits for it.
    ids.pop_back();
    keys.pop_back();
    picks = FindPicks(ids, keys, word);
    EXPECT(picks.named == 2 && picks.away == 1 && (picks.found == std::vector<std::size_t>{0}));

    // Two of one model: the exact path wins even when listed second, and the other pick takes what is left.
    const std::vector<std::wstring> twoIds{L"a#m#1#g", L"a#m#2#g"};
    const std::vector<std::wstring> twoKeys{L"m/dell", L"m/dell"};
    picks = FindPicks(twoIds, twoKeys, L"a#m#2#g~m/dell|a#m#9#g~m/dell");
    EXPECT((picks.found == std::vector<std::size_t>{1, 0}));
    // Each monitor is taken once.
    picks = FindPicks(twoIds, twoKeys, L"a#m#8#g~m/dell|a#m#9#g~m/dell|a#m#7#g~m/dell");
    EXPECT(picks.named == 3 && picks.away == 1 && (picks.found == std::vector<std::size_t>{0, 1}));

    // A monitor named twice is one monitor.
    picks = FindPicks(twoIds, twoKeys, L"a#m#2#g|a#m#2#g");
    EXPECT(picks.named == 2 && picks.away == 1 && (picks.found == std::vector<std::size_t>{1}));

    // A word without identities still finds exact paths; nothing named finds nothing.
    picks = FindPicks(twoIds, twoKeys, L"a#m#2#g");
    EXPECT((picks.found == std::vector<std::size_t>{1}));
    picks = FindPicks(twoIds, twoKeys, L"");
    EXPECT(picks.named == 0 && picks.away == 0 && picks.found.empty());

    EXPECT(NamesWord(std::vector<std::wstring>{}).empty());
    EXPECT(PickWord(L"a", L"") == L"a");
}

/// A row takes no more monitors than the views have columns: a word naming more (written by hand) gives its first ones, and
/// the ones past them are neither in the row nor counted as away.
void MoreNamedThanARowTakes()
{
    const std::vector<std::wstring> ids{L"a", L"b", L"c", L"d", L"e"};
    const std::vector<std::wstring> keys(ids.size());
    auto picks = FindPicks(ids, keys, L"e|d|c|b");
    EXPECT(picks.named == 4 && picks.away == 0 && (picks.found == std::vector<std::size_t>{4, 3, 2}));
    // One of the first three away: the next connected one takes the row's last place.
    picks = FindPicks(ids, keys, L"e|x|c|b");
    EXPECT(picks.named == 4 && picks.away == 1 && (picks.found == std::vector<std::size_t>{4, 2, 1}));
    EXPECT(PickedColumns(picks) == kMostInRow);
}

// Primary first, as Windows lists it: 3440 | 2560 at the right, 2560 at the left, a fourth above.
const std::vector<Monitor> kDesktop{At(0, 0, 3440), At(3440, 0), At(-2560, 0), At(2585, -1440)};

Picks Named(std::int32_t named, std::vector<std::size_t> found)
{
    return {named, named - static_cast<std::int32_t>(found.size()), std::move(found)};
}

/// Named monitors make the row, in the order named, and are never replaced by others.
void TheRowTheyGive()
{
    // None named: the desktop's own row, every column it can have.
    const Picks none;
    EXPECT(PickedColumns(none) == 0);
    Choice choice = WidestRow(kDesktop, none, 0, kMostInRow);
    EXPECT(choice.ok && choice.widths.size() == 3 && (choice.span == Rect{-2560, 0, 6000, 1440}));
    // A desktop whose longest row is two monitors gives two columns when three are asked, and exactly two when asked.
    const std::vector<Monitor> pair{At(0, 0), At(2560, 0), At(0, -1080, 1920, 1080)};
    choice = WidestRow(pair, none, 0, kMostInRow);
    EXPECT(choice.ok && choice.widths.size() == 2);
    EXPECT(!Row(pair, none, 0, 3).ok && Row(pair, none, 0, 2).ok);
    // No row at all: refused with the reason for what was asked.
    const std::vector<Monitor> apart{At(0, 0), At(2560, 0, 1920, 1080)};
    choice = WidestRow(apart, none, 0, kMostInRow);
    EXPECT(!choice.ok && !choice.why.empty());

    // Two named and connected, wherever they sit and in the order named.
    Picks picks = Named(2, {3, 1});
    EXPECT(PickedColumns(picks) == 2);
    choice = WidestRow(kDesktop, picks, 0, kMostInRow);
    EXPECT(choice.ok && (choice.widths == std::vector<std::int32_t>{2560, 2560}) && !choice.moves.empty());
    // Three named, two asked: the first two.
    picks  = Named(3, {2, 0, 1});
    choice = Row(kDesktop, picks, 0, 2);
    EXPECT(choice.ok && (choice.widths == std::vector<std::int32_t>{2560, 3440}));
    EXPECT(WidestRow(kDesktop, picks, 0, kMostInRow).widths.size() == 3);
    EXPECT(WidestRow(kDesktop, picks, 0, 2).widths.size() == 2);  // by the columns asked, not by the most a row takes

    // Fewer than two connected: no row, though the desktop has one of its own; the reason says what is waited for.
    for (const Picks& waiting : {Named(2, {}), Named(2, {0}), Named(1, {0})})
    {
        EXPECT(PickedColumns(waiting) == 0);
        choice = WidestRow(kDesktop, waiting, 0, kMostInRow);
        EXPECT(!choice.ok && choice.why.find(L"waits for at least two") != std::wstring::npos);
        EXPECT(!Row(kDesktop, waiting, 0, 2).ok);
    }
    // Two of three connected: the row of those two; the third joins when it arrives.
    picks  = Named(3, {0, 1});
    choice = WidestRow(kDesktop, picks, 0, kMostInRow);
    EXPECT(choice.ok && choice.widths.size() == 2 && choice.moves.empty());
}

}  // namespace

int main()
{
    PicksFound();
    MoreNamedThanARowTakes();
    TheRowTheyGive();
    std::printf(g_failures == 0 ? "monitor picks: all passed\n" : "monitor picks: %d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
