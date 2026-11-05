// Host-side tests for the Co-op page's rules (menu/coop_menu_rules.h): the page built from the settings table, the
// row bytes kept apart from the game's own settings, the values and words, and the console's readings.

#include "menu/coop_menu_rules.h"

#include <cstdio>
#include <cwchar>
#include <set>
#include <string>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "coop_menu_rules: all passed\n" : "coop_menu_rules: %d failure(s)\n", g_failures);
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

using namespace lss::menu::coop_menu_rules;
namespace settings = lss::platform::settings;
namespace control  = lss::control;

/// Every option has a row name, a short help and one name per word: the page shows nothing the table lacks.
void EveryOptionHasMenuText()
{
    for (const settings::Option* option : settings::kOptions)
    {
        EXPECT(std::wcslen(option->label) > 0);
        EXPECT(std::wcslen(option->menuHelp) > 0);
        EXPECT(option->wordLabels.size() == option->words.size());
        for (const wchar_t* name : option->wordLabels)
        {
            EXPECT(std::wcslen(name) > 0);
        }
    }
}

/// One category per section in the table's order, on the screen's keys (never 5 or 6), every option on exactly one row.
void PageFromTheTable()
{
    const auto page = BuildPage();
    EXPECT(page.size() == 3);
    EXPECT(page.size() <= kCategoryKeys.size());
    std::set<int>          rows;
    std::set<std::uint8_t> keys;
    for (std::size_t i = 0; i < page.size(); ++i)
    {
        EXPECT(page[i].key == kCategoryKeys[i]);
        EXPECT(page[i].key != 5 && page[i].key != 6);
        EXPECT(keys.insert(page[i].key).second);
        EXPECT(!page[i].options.empty());
        EXPECT(std::wcslen(page[i].text.help) > 0);
        for (const int option : page[i].options)
        {
            EXPECT(rows.insert(option).second);
            EXPECT(std::wcscmp(settings::kOptions[static_cast<std::size_t>(option)]->section, page[i].text.section) == 0);
        }
    }
    std::size_t onPage = 0;
    for (const settings::Option* option : settings::kOptions)
    {
        onPage += option->appOnly ? 0 : 1;
    }
    EXPECT(rows.size() == onPage);
    EXPECT(std::wcscmp(page[0].text.label, L"Screens") == 0);
    EXPECT(std::wcscmp(page[2].text.label, L"Experimental") == 0);
    EXPECT(page[0].options == (std::vector<int>{0, 2, 3, 4, 5, 6}));  // 1 is the monitors, set by the setup app
}

/// A row's byte is above every game setting (0..0x34) and names its option back; game bytes name none.
void RowBytesApartFromTheGame()
{
    for (int option = 0; option < static_cast<int>(settings::kOptions.size()); ++option)
    {
        EXPECT(RowByte(option) >= 0x80);
        EXPECT(OptionOf(RowByte(option)) == option);
    }
    for (int game = 0; game <= 0x34; ++game)
    {
        EXPECT(!OptionOf(static_cast<std::uint8_t>(game)).has_value());
    }
    EXPECT(!OptionOf(static_cast<std::uint8_t>(0x7F)).has_value());
    EXPECT(!OptionOf(RowByte(static_cast<int>(settings::kOptions.size()))).has_value());
    EXPECT(!OptionOf(0xFF).has_value());
}

/// Values are the words' places; a value outside them commits nothing; an unknown word has no value.
void ValuesAndWords()
{
    const settings::Option& warps = settings::kBattleWarps;
    EXPECT(ValueOf(warps, L"players-stay") == 0);
    EXPECT(ValueOf(warps, L"game") == 2);
    EXPECT(!ValueOf(warps, L"nonsense").has_value());
    EXPECT(WordOf(warps, 1) == std::wstring(L"off"));
    EXPECT(!WordOf(warps, 3).has_value());
    EXPECT(!WordOf(warps, -1).has_value());
    for (const settings::Option* option : settings::kOptions)
    {
        for (int value = 0; value < static_cast<int>(option->words.size()); ++value)
        {
            EXPECT(ValueOf(*option, *WordOf(*option, value)) == value);
        }
    }
}

/// An on/off option's command takes and reads a bool; any other takes and reads its word.
void CommandValues()
{
    EXPECT(std::get<bool>(CommandValue(control::Shape::OnOff, L"on")));
    EXPECT(!std::get<bool>(CommandValue(control::Shape::OnOff, L"off")));
    EXPECT(std::get<std::wstring>(CommandValue(control::Shape::Word, L"game")) == L"game");
    EXPECT(WordOfReading(control::Value{true}) == std::wstring(L"on"));
    EXPECT(WordOfReading(control::Value{false}) == std::wstring(L"off"));
    EXPECT(WordOfReading(control::Value{std::wstring(L"132")}) == std::wstring(L"132"));
    EXPECT(!WordOfReading(control::Value{std::int64_t{3}}).has_value());
    EXPECT(!WordOfReading(control::Value{}).has_value());
}

/// The rates (100 values) and the split FOV's weight (21) are sliders; the short lists stay lists.
void SlidersAreTheLongRows()
{
    int sliders = 0;
    for (const settings::Option* option : settings::kOptions)
    {
        EXPECT(IsSlider(*option) == (option->wordLabels.size() > kListLongest));
        sliders += IsSlider(*option) ? 1 : 0;
    }
    EXPECT(sliders == 8);  // the two weights, enemy damage, enemy HP, boss HP, stagger, EXP, AP
}

/// The window follows the marker with a cell of context, stays inside the gauge, and is 0 when everything fits.
void SliderWindow()
{
    constexpr int cells = 99, window = kSliderWindow;
    EXPECT(FirstVisible(9, 0, 10, window) == 0);  // a stock gauge: 10 cells, no window
    EXPECT(FirstVisible(0, 0, cells, window) == 0);
    EXPECT(FirstVisible(9, 0, cells, window) == 0);   // the stock place (1.0)
    EXPECT(FirstVisible(10, 0, cells, window) == 1);  // one step on: the window moves one cell
    EXPECT(FirstVisible(50, 0, cells, window) == 41);
    EXPECT(FirstVisible(49, 41, cells, window) == 41);  // back inside: no move
    EXPECT(FirstVisible(42, 41, cells, window) == 41);
    EXPECT(FirstVisible(41, 41, cells, window) == 40);  // within a cell of the left edge
    EXPECT(FirstVisible(99, 41, cells, window) == 89);  // the end: never past the last cell
    EXPECT(FirstVisible(0, 89, cells, window) == 0);    // back to the start
    for (int marker = 0; marker <= cells; ++marker)
    {
        for (const int first : {0, 30, 89})
        {
            const int shown = FirstVisible(marker, first, cells, window);
            EXPECT(shown >= 0 && shown <= cells - window && marker >= shown && marker <= shown + window);
        }
    }
}

}  // namespace

int main()
{
    SlidersAreTheLongRows();
    SliderWindow();
    EveryOptionHasMenuText();
    PageFromTheTable();
    RowBytesApartFromTheGame();
    ValuesAndWords();
    CommandValues();
    return lss::test::Report();
}
