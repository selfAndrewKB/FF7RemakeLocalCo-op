#pragma once

// The Co-op page's rules: what the game's Options screen shows when the mod opens its own copy from the Co-op row of the
// main menu (research/RE-game-widgets.md §9-§10; plan/topics/input.md). The rows are the settings
// table's options (platform/settings.h), one category per section in the table's order, one List row per option with
// its words' names as the values. A row is told apart from the game's own by its category byte: the game's real
// settings live at 0..0x34, the mod's rows take 0x80 + the option's place in the table, which the game never makes, so
// its own value code reads 0 for them and writes nothing (§9.3). Pure: tested on the host (tests/coop_menu_rules_test.cpp).

#include "control/registry.h"
#include "platform/settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace lss::menu::coop_menu_rules {

namespace settings = platform::settings;

/// The main menu's row, in the place of the trial's "Buy Now" row (kind 12, which the full game never shows).
inline constexpr const wchar_t* kRowLabel = L"Co-op";
inline constexpr const wchar_t* kRowHelp  = L"Split-screen co-op settings: the players' screens, battle and experimental options.";
inline constexpr const wchar_t* kTitle    = L"Co-op";

/// The first category byte of the mod's rows; the game's own end below 0x35.
inline constexpr std::uint8_t kFirstRowByte = 0x80;

/// The Options screen's category keys the page fills, in order: keys the game's own build leaves in its map, skipping 5
/// and 6 (the controller and keyboard pages take their own paths, §9.3).
inline constexpr std::array<std::uint8_t, 6> kCategoryKeys{0, 1, 2, 3, 4, 7};

/// A section's name and help on the page.
struct SectionText
{
    const wchar_t* section;
    const wchar_t* label;
    const wchar_t* help;
};
inline constexpr std::array<SectionText, 3> kSections{{
    {L"Views", L"Screens", L"How the players' views are laid out on one or more monitors."},
    {L"Battle", L"Battle", L"How battles run with more than one player."},
    {L"Experimental", L"Experimental", L"Newer options still being tried; their defaults play as before."},
}};

/// The name and help of a section (the section's own name and no help when the list has none).
inline SectionText TextOf(const wchar_t* section) noexcept
{
    for (const SectionText& text : kSections)
    {
        if (std::wcscmp(text.section, section) == 0)
        {
            return text;
        }
    }
    return {section, section, L""};
}

/// One category of the page: its key in the Options screen's map, its name and help, and its rows (indices into
/// settings::kOptions, in the table's order).
struct Category
{
    std::uint8_t     key = 0;
    SectionText      text{};
    std::vector<int> options;
};

/// The page: one category per section, in the order the table first names it. Empty when the table has more sections
/// than the screen has keys for (a programming error the test catches).
inline std::vector<Category> BuildPage()
{
    std::vector<Category> page;
    for (std::size_t i = 0; i < settings::kOptions.size(); ++i)
    {
        if (settings::kOptions[i]->appOnly)
        {
            continue;
        }
        const wchar_t* section = settings::kOptions[i]->section;
        Category*      into    = nullptr;
        for (Category& category : page)
        {
            if (std::wcscmp(category.text.section, section) == 0)
            {
                into = &category;
            }
        }
        if (into == nullptr)
        {
            if (page.size() == kCategoryKeys.size())
            {
                return {};
            }
            page.push_back({kCategoryKeys[page.size()], TextOf(section), {}});
            into = &page.back();
        }
        into->options.push_back(static_cast<int>(i));
    }
    return page;
}

/// A row's category byte.
inline constexpr std::uint8_t RowByte(int option) noexcept
{
    return static_cast<std::uint8_t>(kFirstRowByte + option);
}

/// The option a category byte names, if it is one of the mod's rows.
inline std::optional<int> OptionOf(std::uint8_t rowByte) noexcept
{
    if (rowByte < kFirstRowByte)
    {
        return std::nullopt;
    }
    const int option = rowByte - kFirstRowByte;
    return option < static_cast<int>(settings::kOptions.size()) ? std::optional<int>(option) : std::nullopt;
}

/// The game's longest value list built in code (Resolution, 11 values: research/RE-options-value-list.md §1): its value
/// pull-down cannot scroll, so a row with more values is the game's slider row instead (a Range row over its values'
/// places, 0 to count - 1, the same number a list row passes), shown through a window of the stock gauge's 10 cells
/// that scrolls with the value (research/RE-options-gauge-scroll.md §5).
inline constexpr std::size_t kListLongest  = 11;
inline constexpr int         kSliderWindow = 10;

/// The row is a slider: more values than the game's longest list.
inline bool IsSlider(const settings::Option& option) noexcept
{
    return option.wordLabels.size() > kListLongest;
}

/// The first cell a slider's window shows. `marker` is the marker's place (the value's place: the edge between the
/// filled cells and the rest), `first` the window's first cell so far, `cells` the gauge's cells (count - 1). The window
/// moves only when the marker comes within one cell of its edge (a cell of context kept on either side, as the game's
/// lists scroll to the nearest edge), and never past either end; 0 when every cell fits.
inline int FirstVisible(int marker, int first, int cells, int window) noexcept
{
    if (cells <= window)
    {
        return 0;
    }
    if (marker > first + window - 1)
    {
        first = marker - window + 1;
    }
    if (marker < first + 1)
    {
        first = marker - 1;
    }
    const int last = cells - window;
    return first < 0 ? 0 : (first > last ? last : first);
}

/// The value the page shows for a word: its place in the option's words; none if the word is none of them.
inline std::optional<int> ValueOf(const settings::Option& option, const std::wstring& word) noexcept
{
    for (std::size_t i = 0; i < option.words.size(); ++i)
    {
        if (word == option.words[i])
        {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

/// The word a value the page commits stands for; none when the value is outside the option's words.
inline std::optional<std::wstring> WordOf(const settings::Option& option, int value)
{
    if (value < 0 || value >= static_cast<int>(option.words.size()))
    {
        return std::nullopt;
    }
    return std::wstring(option.words[static_cast<std::size_t>(value)]);
}

/// A word as the option's console command takes it: a bool for an on/off option (control::RegisterOption makes those
/// an OnOff), else the word.
inline control::Value CommandValue(control::Shape shape, const std::wstring& word)
{
    if (shape == control::Shape::OnOff)
    {
        return control::Value{word == L"on"};
    }
    return control::Value{word};
}

/// The word a console command's reading stands for (an OnOff's bool is on/off); none for any other reading.
inline std::optional<std::wstring> WordOfReading(const control::Value& value)
{
    if (const bool* on = std::get_if<bool>(&value))
    {
        return std::wstring(*on ? L"on" : L"off");
    }
    if (const std::wstring* word = std::get_if<std::wstring>(&value))
    {
        return *word;
    }
    return std::nullopt;
}

}  // namespace lss::menu::coop_menu_rules
