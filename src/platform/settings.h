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
    bool                            experimental = false;  // the mod menu shows it as experimental (plan/README.md §9)
    // The Co-op page (menu/coop_menu_rules.h): the row's name, each word's name in `words`' order, a short help line.
    const wchar_t*                  label = L"";
    std::span<const wchar_t* const> wordLabels{};
    const wchar_t*                  menuHelp = L"";
};

// The table. An option's owner (the module named beside it) applies the reading at its install.
inline constexpr std::array<const wchar_t*, 3> kMultiMonitorWords{L"auto", L"on", L"off"};
inline constexpr std::array<const wchar_t*, 3> kMultiMonitorLabels{L"Auto", L"On", L"Off"};
inline constexpr std::array<const wchar_t*, 6> kColumnOrderWords{L"123", L"132", L"213", L"231", L"312", L"321"};
inline constexpr std::array<const wchar_t*, 6> kColumnOrderLabels{L"1 2 3", L"1 3 2", L"2 1 3", L"2 3 1", L"3 1 2", L"3 2 1"};
inline constexpr std::array<const wchar_t*, 2> kMenuSlowWords{L"on", L"off"};
inline constexpr std::array<const wchar_t*, 2> kMenuSlowLabels{L"On", L"Off"};
inline constexpr std::array<const wchar_t*, 3> kBattleWarpsWords{L"players-stay", L"off", L"game"};
inline constexpr std::array<const wchar_t*, 3> kBattleWarpsLabels{L"Players stay", L"Nobody", L"Whole party"};

inline constexpr Option kMultiMonitor{  // views/window_span
    L"Views", L"multi-monitor", kMultiMonitorWords, L"auto",
    L"the game window across a row of equal monitors, one full view per player: auto (while a further player is in), "
    L"on (held with any player count, from the game's next steady frame; a column no player fills mirrors player 1's "
    L"picture), off (no span until window-span on, window-span.hold on or Ctrl+F2; from the console: leaves now)",
    false, L"Multiple monitors", kMultiMonitorLabels,
    L"One full view per player across a row of equal monitors. Auto: while a further player is in. On: always. Off: "
    L"every view in one window."};
inline constexpr Option kColumnOrder{  // views/column_layout
    L"Views", L"column-order", kColumnOrderWords, L"123",
    L"which screen each player takes: the players left to right (in one window, three players: the top, then the bottom "
    L"left and right; two players: top then bottom, or left then right), the player not in skipped; 123 the join order "
    L"(nothing arranged), 132 player 2 on the right and player 3 in the middle, 213 player 2 first and player 1 second",
    false, L"Screen order", kColumnOrderLabels,
    L"Which screen each player takes, left to right (in one window: the top first). 1 2 3: the order they joined. With "
    L"two players the one not playing is skipped."};
inline constexpr Option kMenuSlow{  // battle/command_menu_tick
    L"Battle", L"menu-slow", kMenuSlowWords, L"off",
    L"off (the default): no grey, and the world and the ATB gauges run on at full speed while a battle command menu is "
    L"open; on: an open menu slows the world and greys the screen, as the game does (Ctrl+F9 switches it too)",
    false, L"Command menu slow-down", kMenuSlowLabels,
    L"On: an open battle command menu slows the battle and greys the screen, as in single player. Off: the battle runs "
    L"on at full speed while a player picks a command."};
inline constexpr Option kBattleWarps{  // battle/battle_warps
    L"Battle", L"battle-warps", kBattleWarpsWords, L"players-stay",
    L"who the game warps at a battle's start and end: players-stay (every player's own character stays where it is), "
    L"off (nobody is warped), game (the whole party)",
    false, L"Moves at a battle's start and end", kBattleWarpsLabels,
    L"Who the game moves into place when a battle starts and ends. Players stay: every player's character stays where "
    L"it is. Nobody: no one is moved. Whole party: everyone, as in single player."};
// The battle's rates (battle/battle_rates): each a multiplier from 0.1 to 10.0 in steps of 0.1, written "1.5".
inline constexpr std::size_t kTenthsMax = 100;  // 10.0
using TenthsText = std::array<std::array<wchar_t, 5>, kTenthsMax>;
consteval TenthsText MakeTenthsText()
{
    TenthsText text{};
    for (std::size_t i = 0; i < kTenthsMax; ++i)
    {
        const std::size_t tenths = i + 1;
        std::size_t       at     = 0;
        if (tenths >= 100) text[i][at++] = static_cast<wchar_t>(L'0' + tenths / 100);
        text[i][at++] = static_cast<wchar_t>(L'0' + tenths / 10 % 10);
        text[i][at++] = L'.';
        text[i][at++] = static_cast<wchar_t>(L'0' + tenths % 10);
        text[i][at]   = L'\0';
    }
    return text;
}
inline constexpr TenthsText kTenthsText = MakeTenthsText();
consteval std::array<const wchar_t*, kTenthsMax> MakeTenthsWords()
{
    std::array<const wchar_t*, kTenthsMax> words{};
    for (std::size_t i = 0; i < kTenthsMax; ++i)
    {
        words[i] = kTenthsText[i].data();
    }
    return words;
}
/// "0.1" ... "10.0": the words and the page's values alike.
inline constexpr std::array<const wchar_t*, kTenthsMax> kTenthsWords = MakeTenthsWords();

inline constexpr Option kEnemyDamage{  // battle/battle_rates
    L"Battle", L"enemy-damage", kTenthsWords, L"1.0",
    L"the damage enemies deal to the players' party, times this (0.1 to 10.0, steps of 0.1; 1.0 is the game's); from the "
    L"next hit, with the game's own rounding and its 9999 cap",
    false, L"Enemy damage", kTenthsWords,
    L"Damage enemies deal to the party, times this. 1.0: as the game. From the next hit."};
inline constexpr Option kEnemyHp{  // battle/battle_rates
    L"Battle", L"enemy-hp", kTenthsWords, L"1.0",
    L"every enemy's max HP but the bosses', times this (0.1 to 10.0); enemies in a fight keep their share of HP left",
    false, L"Enemy HP", kTenthsWords,
    L"Every enemy's max HP but the bosses', times this. Enemies already fighting keep the share of HP they have left."};
inline constexpr Option kBossHp{  // battle/battle_rates
    L"Battle", L"boss-hp", kTenthsWords, L"1.0",
    L"the max HP of the enemies the game marks as a fight's leader (bosses, summon bosses, named foes), times this "
    L"(0.1 to 10.0); enemies in a fight keep their share of HP left",
    false, L"Boss HP", kTenthsWords,
    L"The max HP of bosses and named foes, times this. Bosses already fighting keep the share of HP they have left."};
inline constexpr Option kEnemyStagger{  // battle/battle_rates
    L"Battle", L"enemy-stagger", kTenthsWords, L"1.0",
    L"the size of every enemy's stagger gauge, times this (0.1 to 10.0; higher staggers later); enemies in a fight keep "
    L"their gauge's share",
    false, L"Enemy stagger gauge", kTenthsWords,
    L"How much it takes to stagger an enemy, times this. Higher: enemies stagger later."};
inline constexpr Option kExpGain{  // battle/battle_rates
    L"Battle", L"exp-gain", kTenthsWords, L"1.0",
    L"the EXP a won battle gives, times this (0.1 to 10.0), on top of the game's own bonus after the story or with "
    L"2x EXP & 3x AP on; through the game's own bonus rate",
    false, L"EXP gained", kTenthsWords,
    L"EXP from a won battle, times this. Adds to the game's own 2x EXP option."};
inline constexpr Option kApGain{  // battle/battle_rates
    L"Battle", L"ap-gain", kTenthsWords, L"1.0",
    L"the AP a won battle gives, times this (0.1 to 10.0), on top of the game's own bonus after the story or with "
    L"2x EXP & 3x AP on; through the game's own bonus rate",
    false, L"AP gained", kTenthsWords,
    L"AP from a won battle, times this. Adds to the game's own 3x AP option."};

inline constexpr std::array<const wchar_t*, 2> kLeadHolderWords{L"guide", L"keeps"};
inline constexpr std::array<const wchar_t*, 2> kLeadHolderLabels{L"AI guides", L"Player keeps"};
inline constexpr Option kLeadHolder{  // players/lead_gate
    L"Experimental", L"lead-holder", kLeadHolderWords, L"guide",
    L"a lead or run to a mark the level needs finished, on a further player's character: guide (the default: to its AI for "
    L"the whole walk, the player's view on it) or keeps (experimental: the player keeps it, the stop marked in that "
    L"player's view, its AI finishing the arrival within 2 m of the stop)",
    true, L"Leading the way", kLeadHolderLabels,
    L"When the story has another player's character lead the way. AI guides: its AI walks there and the player watches. "
    L"Player keeps: the player walks it to the marked stop."};

inline constexpr std::array<const Option*, 11> kOptions{&kMultiMonitor, &kColumnOrder, &kMenuSlow, &kBattleWarps,
                                                       &kEnemyDamage, &kEnemyHp, &kBossHp, &kEnemyStagger, &kExpGain,
                                                       &kApGain, &kLeadHolder};

/// The option is a row of the table (Of and Why answer by the row).
consteval bool Listed(const Option& option)
{
    for (const Option* row : kOptions)
    {
        if (row == &option) return true;
    }
    return false;
}
static_assert(Listed(kMultiMonitor) && Listed(kColumnOrder) && Listed(kMenuSlow) && Listed(kBattleWarps) &&
                  Listed(kEnemyDamage) && Listed(kEnemyHp) && Listed(kBossHp) && Listed(kEnemyStagger) && Listed(kExpGain) &&
                  Listed(kApGain) && Listed(kLeadHolder),
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
