#pragma once

// The mod's own settings file: LocalSplitScreen.ini in the mod's folder (Mods/LocalSplitScreen/LocalSplitScreen.ini,
// one level above the dlls folder this module is loaded from), read with the Windows profile API, so a user sets a
// preset once without the console. The options are one table here (the section and key in the file, the words the
// option takes, the word taken when the file has none, one line of help). Load reads every option once at the
// mod's start: one warning per word the file gets wrong, in one format, and one Info line with every option's word
// and where it came from. Each option's owner applies its reading at install through the one setter the console
// and the keys call too, and control::RegisterOption builds the console command from the table, so an option is
// described once. A word is read trimmed, lower-cased and with its spaces and commas removed (`1 3 2`, `1, 3, 2`
// and `132` are one word).
// A missing file or key is the default; nothing is written to the file. Judge, WordList, WhyText and InvalidText
// need no game and are tested on the host (tests/settings_test.cpp). Game thread.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <span>
#include <string>
#include <string_view>

namespace lss::platform::settings {

/// One option of the file, as the table lists it.
struct Option
{
    const wchar_t*                  section;   // the [Section] in the file
    const wchar_t*                  key;       // the key in the file, and the console command's name
    std::span<const wchar_t* const> words;     // the words the option takes, lower case, in the help's order
    const wchar_t*                  fallback;  // the word taken when the file has none: one of `words`
    const wchar_t*                  help;      // one line, the console's help
};

// The table. An option's owner (the module named beside it) applies the reading at its install.
inline constexpr std::array<const wchar_t*, 3> kMultiMonitorWords{L"auto", L"on", L"off"};
inline constexpr std::array<const wchar_t*, 2> kColumnOrderWords{L"123", L"132"};
inline constexpr std::array<const wchar_t*, 2> kMenuSlowWords{L"on", L"off"};
inline constexpr std::array<const wchar_t*, 3> kBattleWarpsWords{L"players-stay", L"off", L"game"};

inline constexpr Option kMultiMonitor{  // views/window_span
    L"Views", L"multi-monitor", kMultiMonitorWords, L"auto",
    L"the game window across a row of equal monitors, one full view per player: auto (while a further player is in), "
    L"on (held with any player count, from the game's next steady frame; a column no player fills mirrors player 1's "
    L"picture), off (no span until window-span on, window-span.hold on or Ctrl+F2; from the console: leaves now)"};
inline constexpr Option kColumnOrder{  // views/column_layout
    L"Views", L"column-order", kColumnOrderWords, L"123",
    L"which screen players 2 and 3 take when three views show: 123 the join order, 132 player 2 on the right and player "
    L"3 in the middle (in one window: bottom right and bottom left); player 1 keeps the left"};
inline constexpr Option kMenuSlow{  // battle/command_menu_tick
    L"Battle", L"menu-slow", kMenuSlowWords, L"off",
    L"off (the default): no grey, and the world and the ATB gauges run on at full speed while a battle command menu is "
    L"open; on: an open menu slows the world and greys the screen, as the game does (Ctrl+F9 switches it too)"};
inline constexpr Option kBattleWarps{  // battle/battle_warps
    L"Battle", L"battle-warps", kBattleWarpsWords, L"players-stay",
    L"who the game warps at a battle's start and end: players-stay (every player's own character stays where it is), "
    L"off (nobody is warped), game (the whole party)"};

inline constexpr std::array<const Option*, 4> kOptions{&kMultiMonitor, &kColumnOrder, &kMenuSlow, &kBattleWarps};

/// The option is a row of the table (Of and Why answer by the row).
consteval bool Listed(const Option& option)
{
    for (const Option* row : kOptions)
    {
        if (row == &option) return true;
    }
    return false;
}
static_assert(Listed(kMultiMonitor) && Listed(kColumnOrder) && Listed(kMenuSlow) && Listed(kBattleWarps),
              "every option is a row of kOptions");

/// Where an option's word came from.
enum class Source : std::uint8_t
{
    Default,  // the file has no such key (or no file): the option's fallback
    File,     // the file's word, one of the option's
    Invalid,  // the file's word is none of the option's: the fallback, with a warning
};

struct Reading
{
    std::wstring word;                      // one of the option's words
    Source       source = Source::Default;
    std::wstring raw;                       // the file's text as written (trimmed), kept for the warning when Invalid
};

/// The file's text for an option, judged: trimmed, lower-cased and with its spaces and commas removed, then matched
/// against the option's words; empty is the default.
inline Reading Judge(const Option& option, std::wstring_view fileText)
{
    std::wstring word;
    for (const wchar_t c : fileText)
    {
        if (!std::iswspace(c) && c != L',')
        {
            word.push_back(static_cast<wchar_t>(std::towlower(c)));
        }
    }
    if (word.empty())
    {
        return {option.fallback, Source::Default, {}};
    }
    for (const wchar_t* allowed : option.words)
    {
        if (word == allowed)
        {
            return {word, Source::File, {}};
        }
    }
    std::size_t first = 0, last = fileText.size();
    while (first < last && std::iswspace(fileText[first])) ++first;
    while (last > first && std::iswspace(fileText[last - 1])) --last;
    return {option.fallback, Source::Invalid, std::wstring(fileText.substr(first, last - first))};
}

/// The option's words as a list: "auto, on or off".
inline std::wstring WordList(const Option& option)
{
    std::wstring list;
    const std::size_t count = option.words.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        list += option.words[i];
        if (i + 2 < count) list += L", ";
        else if (i + 2 == count) list += L" or ";
    }
    return list;
}

/// The warning Load logs for an invalid word: "LocalSplitScreen.ini [Battle] menu-slow = 'x' is not on or off; read
/// as on".
inline std::wstring InvalidText(const Option& option, const Reading& reading)
{
    return std::wstring(L"LocalSplitScreen.ini [") + option.section + L"] " + option.key + L" = '" + reading.raw +
           L"' is not " + WordList(option) + L"; read as " + option.fallback;
}

/// Why a reading reads as it does, for the owner's own log line: "the settings file", "the default; LocalSplitScreen.ini
/// has no [Battle] menu-slow", "the default; LocalSplitScreen.ini [Battle] menu-slow = 'x' is not on or off".
inline std::wstring WhyText(const Option& option, const Reading& reading)
{
    switch (reading.source)
    {
        case Source::File:
            return L"the settings file";
        case Source::Invalid:
            return std::wstring(L"the default; LocalSplitScreen.ini [") + option.section + L"] " + option.key + L" = '" +
                   reading.raw + L"' is not " + WordList(option);
        case Source::Default:
        default:
            return std::wstring(L"the default; LocalSplitScreen.ini has no [") + option.section + L"] " + option.key;
    }
}

/// The file's path, from this module's own path; empty if the module's path cannot be read.
std::wstring Path();

/// Reads every option of the table from the file, once, before any module installs (game thread). A missing file
/// leaves every option at its default. Logs one warning per invalid word and one Info line with every reading.
void Load();

/// An option's reading (its default before Load). An option that is not a row of the table (a programming error,
/// caught by Listed's static_assert for the rows declared here) answers its own fallback, with a warning once.
const Reading& Of(const Option& option) noexcept;

/// WhyText(option, Of(option)).
std::wstring Why(const Option& option);

}  // namespace lss::platform::settings
