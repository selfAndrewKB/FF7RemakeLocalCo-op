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
// A missing file or key is the default. The file is the table's own: Load writes it from the table after reading it
// (platform/settings_write.h: every option with its Co-op page name, help, words and default, the values kept, a key
// the table no longer has dropped), so nothing in it is a hand-kept copy that can go stale; the repository ships no
// copy. The Co-op page saves each choice it applies the same way (Save); the console and the keys change an option for
// the session only. Judge, WordList, WhyText, InvalidText and the file text and write need no game and are tested on
// the host (tests/settings_test.cpp). Game thread.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <span>
#include <string>
#include <string_view>

namespace lss::platform::settings {

/// The settings file's name, in the mod's folder.
inline constexpr const wchar_t* kFileName = L"LocalSplitScreen.ini";

/// One option of the file, as the table lists it.
struct Option
{
    const wchar_t*                  section;   // the [Section] in the file
    const wchar_t*                  key;       // the key in the file, and the console command's name
    std::span<const wchar_t* const> words;     // the words the option takes, lower case, in the help's order
    const wchar_t*                  fallback;  // the word taken when the file has none: one of `words`
    const wchar_t*                  help;      // one line, the console's help
    bool                            experimental = false;  // the mod menu shows it as experimental
    // The Co-op page (menu/coop_menu_rules.h): the row's name, each word's name in `words`' order, a short help line.
    const wchar_t*                  label = L"";
    std::span<const wchar_t* const> wordLabels{};
    const wchar_t*                  menuHelp = L"";
    // Set by the setup app, no row on the Co-op page: `words` is empty and the option takes any word, an empty one its default.
    bool                            appOnly = false;
};

// The table. An option's owner (the module named beside it) applies the reading at its install.
inline constexpr std::array<const wchar_t*, 6> kColumnOrderWords{L"123", L"132", L"213", L"231", L"312", L"321"};
inline constexpr std::array<const wchar_t*, 6> kColumnOrderLabels{L"1 2 3", L"1 3 2", L"2 1 3", L"2 3 1", L"3 1 2", L"3 2 1"};
inline constexpr std::array<const wchar_t*, 2> kTwoPlayerSplitWords{L"vertical", L"horizontal"};
inline constexpr std::array<const wchar_t*, 2> kTwoPlayerSplitLabels{L"Vertical", L"Horizontal"};
inline constexpr std::array<const wchar_t*, 4> kThreePlayerSplitWords{L"top", L"bottom", L"left", L"right"};
inline constexpr std::array<const wchar_t*, 4> kThreePlayerSplitLabels{L"One on Top, Two on Bottom", L"One on Bottom, Two on Top",
                                                                       L"One on Left, Two on Right", L"One on Right, Two on Left"};
inline constexpr std::array<const wchar_t*, 2> kOnOffWords{L"on", L"off"};
inline constexpr std::array<const wchar_t*, 2> kOnOffLabels{L"On", L"Off"};
inline constexpr std::array<const wchar_t*, 2> kAreaLevelsWords{L"off", L"on"};
inline constexpr std::array<const wchar_t*, 2> kAreaLevelsLabels{L"Only Player 1", L"All Players"};
inline constexpr std::array<const wchar_t*, 3> kBattleWarpsWords{L"players-stay", L"off", L"game"};
inline constexpr std::array<const wchar_t*, 3> kBattleWarpsLabels{L"Players Stay", L"Nobody", L"Whole Party"};

inline constexpr Option kMultiMonitor{  // views/window_span
    L"Views", L"multi-monitor", kOnOffWords, L"off",
    L"the game window across a row of monitors, one full view per player, each fitted to its monitor: "
    L"on (held with any player count, from the game's next steady frame; a column no player fills mirrors player 1's "
    L"picture), off (no span until window-span on, window-span.hold on or Ctrl+F2; from the console: leaves now)",
    false, L"Multiple Monitors", kOnOffLabels,
    L"One full view per player across a row of monitors, each fitted to its monitor. On: always. Off: "
    L"every view in one window."};
inline constexpr Option kMonitors{  // views/window_span, views/monitor_picks FindPicks
    L"Views", L"monitors", {}, L"",
    L"(read only) the monitors of the row, left to right, as the setup app wrote them; empty: any row the desktop has",
    false, L"Monitors in the Row", {},
    L"The monitors of the row, left to right, chosen in the Multi Monitor Setup app; they are moved into a row while the game holds it and back afterwards. Empty: the longest row the desktop has.",
    true};
inline constexpr Option kColumnOrder{  // views/column_layout
    L"Views", L"column-order", kColumnOrderWords, L"123",
    L"which screen each player takes: the players left to right (in one window, three players: the large view, then the "
    L"other two, top or left first; two players: top then bottom, or left then right), by player number whatever the "
    L"order they joined in, the player not in skipped: 123 players 1, 2, 3, 132 player 2 on the right and player 3 in the "
    L"middle, 213 player 2 first and player 1 second",
    false, L"Screen Order", kColumnOrderLabels,
    L"Which screen each player takes, left to right. In one window with three players the first takes the large view. "
    L"With two players the one not playing is skipped."};
inline constexpr Option kTwoPlayerSplit{  // views/column_layout
    L"Views", L"two-player-split", kTwoPlayerSplitWords, L"horizontal",
    L"two players in one window: vertical (side by side) or horizontal (top and bottom); a span of monitors is always side "
    L"by side",
    false, L"2-Player Split-Screen Layout", kTwoPlayerSplitLabels,
    L"Vertical: side by side. Horizontal: one above the other."};
inline constexpr Option kThreePlayerSplit{  // views/column_layout
    L"Views", L"three-player-split", kThreePlayerSplitWords, L"top",
    L"three players in one window: top (one across the top, two below), bottom (two above, one across the bottom), left "
    L"(one down the left, two stacked on the right) or right (two stacked on the left, one down the right); the screen "
    L"order's first player takes the large view; a span of monitors is always columns",
    false, L"3-Player Split-Screen Layout", kThreePlayerSplitLabels,
    L"How three players share one screen. The first player in the screen order takes the large view."};
inline constexpr Option kMenuSlow{  // battle/command_menu_tick
    L"Battle", L"menu-slow", kOnOffWords, L"off",
    L"off (the default): no grey and no muffled sound, and the world and the ATB gauges run on at full speed while a battle "
    L"command menu is open; on: an open menu slows the world, greys the screen and muffles the sound, as the game does "
    L"(Ctrl+F9 switches it too)",
    false, L"Command Menu Slow-Down", kOnOffLabels,
    L"On: an open battle command menu slows the battle, greys the screen and muffles the sound, as in single player. Off: "
    L"the battle runs on at full speed and full sound while a player picks a command."};
inline constexpr Option kBattleWarps{  // battle/battle_warps
    L"Battle", L"battle-warps", kBattleWarpsWords, L"players-stay",
    L"who the game warps at a battle's start and end: players-stay (every player's own character stays where it is), "
    L"off (nobody is warped), game (the whole party)",
    false, L"Moves at a Battle's Start and End", kBattleWarpsLabels,
    L"Who the game moves into place when a battle starts and ends. Players Stay: every player's character stays where "
    L"it is. Nobody: no one is moved. Whole Party: everyone, as in single player."};
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

// The split FOV's weight (views/split_fov_rules.h): 0 to 1 in steps of 0.05, written "0.35".
inline constexpr std::size_t kFovWeightSteps = 21;
using FovWeightText = std::array<std::array<wchar_t, 5>, kFovWeightSteps>;
consteval FovWeightText MakeFovWeightText()
{
    FovWeightText text{};
    for (std::size_t i = 0; i < kFovWeightSteps; ++i)
    {
        const std::size_t hundredths = i * 5;
        text[i][0] = static_cast<wchar_t>(L'0' + hundredths / 100);
        text[i][1] = L'.';
        text[i][2] = static_cast<wchar_t>(L'0' + hundredths / 10 % 10);
        text[i][3] = static_cast<wchar_t>(L'0' + hundredths % 10);
        text[i][4] = L'\0';
    }
    return text;
}
inline constexpr FovWeightText kFovWeightText = MakeFovWeightText();
consteval std::array<const wchar_t*, kFovWeightSteps> MakeFovWeightWords()
{
    std::array<const wchar_t*, kFovWeightSteps> words{};
    for (std::size_t i = 0; i < kFovWeightSteps; ++i)
    {
        words[i] = kFovWeightText[i].data();
    }
    return words;
}
/// "0.00" ... "1.00": the words and the page's values alike.
inline constexpr std::array<const wchar_t*, kFovWeightSteps> kFovWeightWords = MakeFovWeightWords();

inline constexpr Option kHorizontalFov{  // views/split_fov
    L"Views", L"split-fov.horizontal", kFovWeightWords, L"0.50",
    L"how a view wider than the game's image shares the difference (two players top and bottom, the large view of three "
    L"with one on top or on the bottom): 0.00 keeps the single-player height (the whole picture's height shown: everything "
    L"smaller, more to the sides), 1.00 the single-player size (as wide as in single player, cut at the knees), 0.50 the "
    L"middle; steps of 0.05",
    false, L"Horizontal Split-Screen FOV", kFovWeightWords,
    L"For views wider than the game's picture. Low: more of the scene, everything smaller. High: characters at their usual size, cut off "
    L"lower down. 0.50: the middle."};
inline constexpr Option kVerticalFov{  // views/split_fov
    L"Views", L"split-fov.vertical", kFovWeightWords, L"1.00",
    L"how a full-height view narrower than the game's image shares the difference (two players side by side, the large view of "
    L"three with one on the left or on the right): 0.00 keeps the single-player width (the whole picture's width shown: "
    L"everything smaller), 1.00 the single-player size (as tall as in single player, the sides cropped); steps of 0.05; a "
    L"narrower view also softens the camera's sideways offset (views/narrow_camera)",
    false, L"Vertical Split-Screen FOV", kFovWeightWords,
    L"For full-height views narrower than the game's picture. Low: more of the scene, everything smaller. High: characters at "
    L"their usual size, the sides cropped."};

inline constexpr Option kEnemyDamage{  // battle/battle_rates
    L"Battle", L"enemy-damage", kTenthsWords, L"1.0",
    L"the damage enemies deal to the players' party, times this (0.1 to 10.0, steps of 0.1; 1.0 is the game's); from the "
    L"next hit, with the game's own rounding and its 9999 cap",
    false, L"Enemy Damage", kTenthsWords,
    L"Damage enemies deal to the party, times this. 1.0: as the game. From the next hit."};
inline constexpr Option kEnemyHp{  // battle/battle_rates
    L"Battle", L"enemy-hp", kTenthsWords, L"1.0",
    L"every enemy's max HP but the bosses', times this (0.1 to 10.0); from the next fight",
    false, L"Enemy HP", kTenthsWords,
    L"Every enemy's max HP but the bosses', times this. From the next fight."};
inline constexpr Option kBossHp{  // battle/battle_rates
    L"Battle", L"boss-hp", kTenthsWords, L"1.0",
    L"the max HP of the enemies the game marks as a fight's leader (bosses, summon bosses, named foes), times this "
    L"(0.1 to 10.0); from the next fight",
    false, L"Boss HP", kTenthsWords,
    L"The max HP of bosses and named foes, times this. From the next fight."};
inline constexpr Option kEnemyStagger{  // battle/battle_rates
    L"Battle", L"enemy-stagger", kTenthsWords, L"1.0",
    L"the size of every enemy's stagger gauge, times this (0.1 to 10.0; higher staggers later); from the next fight",
    false, L"Enemy Stagger Gauge", kTenthsWords,
    L"How much it takes to stagger an enemy, times this. Higher: enemies stagger later. From the next fight."};
inline constexpr Option kExpGain{  // battle/battle_rates
    L"Battle", L"exp-gain", kTenthsWords, L"1.0",
    L"the EXP a won battle gives, times this (0.1 to 10.0), on top of the game's own bonus after the story or with "
    L"2x EXP & 3x AP on; through the game's own bonus rate",
    false, L"EXP Gained", kTenthsWords,
    L"EXP from a won battle, times this. Adds to the game's own 2x EXP option."};
inline constexpr Option kApGain{  // battle/battle_rates
    L"Battle", L"ap-gain", kTenthsWords, L"1.0",
    L"the AP a won battle gives, times this (0.1 to 10.0), on top of the game's own bonus after the story or with "
    L"2x EXP & 3x AP on; through the game's own bonus rate",
    false, L"AP Gained", kTenthsWords,
    L"AP from a won battle, times this. Adds to the game's own 3x AP option."};

inline constexpr std::array<const wchar_t*, 2> kLeadHolderWords{L"guide", L"keeps"};
inline constexpr std::array<const wchar_t*, 2> kLeadHolderLabels{L"AI Guides", L"Player Keeps"};
inline constexpr Option kLeadHolder{  // players/lead_gate
    L"Experimental", L"lead-holder", kLeadHolderWords, L"guide",
    L"a lead or run to a mark the level needs finished, on a further player's character: guide (the default: to its AI for "
    L"the whole walk, the player's view on it) or keeps (experimental: the player keeps it, the stop marked in that "
    L"player's view, its AI finishing the arrival within 2 m of the stop)",
    true, L"Leading the Way", kLeadHolderLabels,
    L"When the story has another player's character lead the way. AI Guides: its AI walks there and the player watches. "
    L"Player Keeps: the player walks it to the marked stop."};
inline constexpr Option kAreaLevels{  // world/area_levels
    L"Experimental", L"area-levels", kAreaLevelsWords, L"on",
    L"on: every player's area stays loaded and solid, not only player 1's, at the cost of more memory (each further "
    L"player's area stays loaded beside player 1's); off: the world follows player 1's area alone; from the next request",
    true, L"World Loading", kAreaLevelsLabels,
    L"Only Player 1: the world loads only around player 1's immediate area. All Players: the world loads around every player's "
    L"position, preventing the world from unloading when no longer around player 1. Uses more memory."};

/// Every option, in the order the settings file lists them.
inline constexpr std::array kOptions{&kMultiMonitor, &kMonitors,    &kColumnOrder,  &kTwoPlayerSplit, &kThreePlayerSplit, &kHorizontalFov,
                                     &kVerticalFov,  &kMenuSlow,    &kBattleWarps,  &kEnemyDamage,    &kEnemyHp,          &kBossHp,
                                     &kEnemyStagger, &kExpGain,     &kApGain,       &kLeadHolder,     &kAreaLevels};

/// The option is a row of the table (Of and Why answer by the row).
consteval bool Listed(const Option& option)
{
    for (const Option* row : kOptions)
    {
        if (row == &option) return true;
    }
    return false;
}
static_assert(Listed(kMultiMonitor) && Listed(kMonitors) && Listed(kColumnOrder) && Listed(kTwoPlayerSplit) && Listed(kThreePlayerSplit) &&
                  Listed(kHorizontalFov) && Listed(kVerticalFov) && Listed(kMenuSlow) && Listed(kBattleWarps) && Listed(kEnemyDamage) && Listed(kEnemyHp) && Listed(kBossHp) &&
                  Listed(kEnemyStagger) && Listed(kExpGain) && Listed(kApGain) && Listed(kLeadHolder) && Listed(kAreaLevels),
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
    if (option.words.empty())
    {
        return {word, Source::File, {}};
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
/// leaves every option at its default. Logs one warning per invalid word and one Info line with every reading. Then
/// writes the file from the table with those readings, unless it already reads exactly so (logged at Info, with any key
/// dropped).
void Load();

/// An option's reading (its default before Load). An option that is not a row of the table (a programming error,
/// caught by Listed's static_assert for the rows declared here) answers its own fallback, with a warning once.
const Reading& Of(const Option& option) noexcept;

/// WhyText(option, Of(option)).
std::wstring Why(const Option& option);

/// Writes the file from the table with `word` (one of the option's words) for `option`, so the next start reads it,
/// and makes it the option's reading (from the file). Logs the save (Info) or why it failed (Warning). False if the word is none of the
/// option's, the option is not a row of the table, the file's path is unknown or the write failed.
bool Save(const Option& option, const std::wstring& word);

}  // namespace lss::platform::settings
