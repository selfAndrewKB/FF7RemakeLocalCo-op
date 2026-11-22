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
    // Set in the file only, no row on the Co-op page: read once at the mod's start.
    bool                            fileOnly = false;
    // A row in the game's own Options screen instead (Camera & Controls, after the game's Responsiveness): a Range row
    // over `words`, its values counted from 1 (menu/coop_menu_rules.h RowMin).
    bool                            gameScreen = false;
};

/// The option has a row on the Co-op page.
constexpr bool OnCoopPage(const Option& option) noexcept
{
    return !option.appOnly && !option.fileOnly && !option.gameScreen;
}

/// The option has a row in the game's own Options screen.
constexpr bool OnGameScreen(const Option& option) noexcept
{
    return option.gameScreen;
}

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

inline constexpr Option kMultiMonitor{  // views/window_span
    L"Views", L"multi-monitor", kOnOffWords, L"off",
    L"the game window across a row of monitors, one full view per player, each fitted to its monitor: "
    L"on (held with any player count, from the game's next steady frame; a column no player fills mirrors player 1's "
    L"picture), off (no span until window-span on, window-span.hold on or Ctrl+F2; from the console: leaves now)",
    false, L"Multiple Monitors", kOnOffLabels,
    L"Controls whether every player has their own display."};
inline constexpr Option kMonitors{  // views/window_span, views/monitor_picks FindPicks
    L"Views", L"monitors", {}, L"",
    L"(read only) the monitors of the row, left to right, as the setup app wrote them; empty: any row the desktop has",
    false, L"Monitors in the Row", {},
    L"The monitors used for Multiple Monitors, left to right, picked in the Multi Monitor Setup app. Empty: the game picks "
    L"the longest row it finds.",
    true};
inline constexpr Option kColumnOrder{  // views/column_layout
    L"Views", L"column-order", kColumnOrderWords, L"123",
    L"which screen each player takes: the players left to right (in one window, three players: the large view, then the "
    L"other two, top or left first; two players: top then bottom, or left then right), by player number whatever the "
    L"order they joined in, the player not in skipped: 123 players 1, 2, 3, 132 player 2 on the right and player 3 in the "
    L"middle, 213 player 2 first and player 1 second",
    false, L"Screen Order", kColumnOrderLabels,
    L"Controls the order in which Players 1, 2 and 3's screens are arranged. Also affects multi-monitor mode."};
inline constexpr Option kTwoPlayerSplit{  // views/column_layout
    L"Views", L"two-player-split", kTwoPlayerSplitWords, L"horizontal",
    L"two players in one window: vertical (side by side) or horizontal (top and bottom); a span of monitors is always side "
    L"by side",
    false, L"2-Player Split-Screen Layout", kTwoPlayerSplitLabels,
    L"Vertical: side by side. Horizontal: top and bottom."};
inline constexpr Option kThreePlayerSplit{  // views/column_layout
    L"Views", L"three-player-split", kThreePlayerSplitWords, L"top",
    L"three players in one window: top (one across the top, two below), bottom (two above, one across the bottom), left "
    L"(one down the left, two stacked on the right) or right (two stacked on the left, one down the right); the screen "
    L"order's first player takes the large view; a span of monitors is always columns",
    false, L"3-Player Split-Screen Layout", kThreePlayerSplitLabels,
    L"Various layouts for 3-player split-screen."};
inline constexpr Option kMenuSlow{  // battle/command_menu_tick
    L"Battle", L"menu-slow", kOnOffWords, L"off",
    L"off (the default): no grey and no muffled sound, and the world and the ATB gauges run on at full speed while a battle "
    L"command menu is open; on: an open menu slows the world, greys the screen and muffles the sound, as the game does "
    L"(Ctrl+F9 switches it too)",
    false, L"Command Menu Slow-Down", kOnOffLabels,
    L"Whether opening the command menu during battle slows down the game."};
inline constexpr Option kBattleWarps{  // battle/battle_warps
    L"Battle", L"battle-warps", kOnOffWords, L"off",
    L"whether the game warps the players' characters into place at a battle's start and end: on (the whole party, as in "
    L"single player) or off (the default: players 2 and 3's characters stay where they are; the members no player "
    L"controls are warped as the game does)",
    false, L"Gather at Battle Start/End", kOnOffLabels,
    L"Whether the game teleports players into place at the beginning and end of a battle encounter."};
// The battle's rates (battle/battle_rates): each a multiplier from 0.1 to 10.0 in steps of 0.1, written "1.5" in the file
// and "x1.5" on the Co-op page.
inline constexpr std::size_t kTenthsMax = 100;  // 10.0
using TenthsText = std::array<std::array<wchar_t, 6>, kTenthsMax>;
/// Each value's text, after `prefix` when it is not 0.
consteval TenthsText MakeTenthsText(wchar_t prefix)
{
    TenthsText text{};
    for (std::size_t i = 0; i < kTenthsMax; ++i)
    {
        const std::size_t tenths = i + 1;
        std::size_t       at     = 0;
        if (prefix != 0) text[i][at++] = prefix;
        if (tenths >= 100) text[i][at++] = static_cast<wchar_t>(L'0' + tenths / 100);
        text[i][at++] = static_cast<wchar_t>(L'0' + tenths / 10 % 10);
        text[i][at++] = L'.';
        text[i][at++] = static_cast<wchar_t>(L'0' + tenths % 10);
        text[i][at]   = L'\0';
    }
    return text;
}
inline constexpr TenthsText kTenthsText      = MakeTenthsText(0);
inline constexpr TenthsText kTenthsTimesText = MakeTenthsText(L'x');
static_assert(kTenthsTimesText[kTenthsMax - 1][5] == L'\0' && kTenthsTimesText[kTenthsMax - 1][4] == L'0',
              "the longest value, x10.0, fits its text with its end");
consteval std::array<const wchar_t*, kTenthsMax> MakeTenthsWords(const TenthsText& text)
{
    std::array<const wchar_t*, kTenthsMax> words{};
    for (std::size_t i = 0; i < kTenthsMax; ++i)
    {
        words[i] = text[i].data();
    }
    return words;
}
/// "0.1" ... "10.0": the words.
inline constexpr std::array<const wchar_t*, kTenthsMax> kTenthsWords = MakeTenthsWords(kTenthsText);
/// "x0.1" ... "x10.0": the page's values, read as "times".
inline constexpr std::array<const wchar_t*, kTenthsMax> kTenthsLabels = MakeTenthsWords(kTenthsTimesText);

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
    L"Controls the field of view when playing in horizontal split-screen. Also affects the largest view during 3-player. "
    L"Lower values show more of the scene."};
inline constexpr Option kVerticalFov{  // views/split_fov
    L"Views", L"split-fov.vertical", kFovWeightWords, L"1.00",
    L"how a full-height view narrower than the game's image shares the difference (two players side by side, the large view of "
    L"three with one on the left or on the right): 0.00 keeps the single-player width (the whole picture's width shown: "
    L"everything smaller), 1.00 the single-player size (as tall as in single player, the sides cropped); steps of 0.05; a "
    L"narrower view also softens the camera's sideways offset (views/narrow_camera)",
    false, L"Vertical Split-Screen FOV", kFovWeightWords,
    L"Controls the field of view when playing in vertical split-screen. Also affects the largest view during 3-player. "
    L"Lower values show more of the scene."};

// Each player's HUD size in their own view in a split window (hud/root_placement): 0.75 to 1.50 in steps of 0.05,
// written "1.25" in the file and "x1.25" on the Co-op page.
inline constexpr std::size_t kHudScaleSteps = 16;
using HudScaleText = std::array<std::array<wchar_t, 6>, kHudScaleSteps>;
/// The factor of the i-th word.
constexpr float HudScaleOf(std::size_t i) noexcept
{
    return static_cast<float>(75 + 5 * i) / 100.0f;
}
consteval HudScaleText MakeHudScaleText(bool times)
{
    HudScaleText text{};
    for (std::size_t i = 0; i < kHudScaleSteps; ++i)
    {
        const std::size_t hundredths = 75 + i * 5;
        std::size_t       at         = 0;
        if (times) text[i][at++] = L'x';
        text[i][at++] = static_cast<wchar_t>(L'0' + hundredths / 100);
        text[i][at++] = L'.';
        text[i][at++] = static_cast<wchar_t>(L'0' + hundredths / 10 % 10);
        text[i][at++] = static_cast<wchar_t>(L'0' + hundredths % 10);
        text[i][at]   = L'\0';
    }
    return text;
}
inline constexpr HudScaleText kHudScaleText      = MakeHudScaleText(false);
inline constexpr HudScaleText kHudScaleTimesText = MakeHudScaleText(true);
consteval std::array<const wchar_t*, kHudScaleSteps> MakeHudScaleWords(const HudScaleText& text)
{
    std::array<const wchar_t*, kHudScaleSteps> words{};
    for (std::size_t i = 0; i < kHudScaleSteps; ++i)
    {
        words[i] = text[i].data();
    }
    return words;
}
/// "0.75" ... "1.50": the words.
inline constexpr std::array<const wchar_t*, kHudScaleSteps> kHudScaleWords = MakeHudScaleWords(kHudScaleText);
/// "x0.75" ... "x1.50": the page's values.
inline constexpr std::array<const wchar_t*, kHudScaleSteps> kHudScaleLabels = MakeHudScaleWords(kHudScaleTimesText);

inline constexpr Option kHudScale{  // hud/root_placement
    L"Views", L"hud-scale", kHudScaleWords, L"1.00",
    L"player 1's size of the HUD pinned to their view's edges in a split window (the minimap and compass, the command "
    L"menu, the party panel, the summon gauges), times this, each about its own corner: 0.75 to 1.50 in steps of 0.05 "
    L"(1.00 is the game's); what hangs over characters and enemies keeps the game's size, as do the single view, the "
    L"menus and scenes and a row of monitors; from the next frame",
    false, L"Split-Screen HUD Size (Player 1)", kHudScaleLabels,
    L"Controls the size of the HUD in Player 1's view during single monitor split-screen mode."};
inline constexpr Option kHudScaleTwo{  // hud/root_placement
    L"Views", L"hud-scale.player-2", kHudScaleWords, L"1.00",
    L"player 2's HUD size in their own view in a split window, as hud-scale sets player 1's: 0.75 to 1.50 in steps of 0.05 "
    L"(1.00 is the game's); a file that has only hud-scale gives player 2 that size once; from the next frame",
    false, L"Split-Screen HUD Size (Player 2)", kHudScaleLabels,
    L"Controls the size of the HUD in Player 2's view during single monitor split-screen mode."};
inline constexpr Option kHudScaleThree{  // hud/root_placement
    L"Views", L"hud-scale.player-3", kHudScaleWords, L"1.00",
    L"player 3's HUD size in their own view in a split window, as hud-scale sets player 1's: 0.75 to 1.50 in steps of 0.05 "
    L"(1.00 is the game's); a file that has only hud-scale gives player 3 that size once; from the next frame",
    false, L"Split-Screen HUD Size (Player 3)", kHudScaleLabels,
    L"Controls the size of the HUD in Player 3's view during single monitor split-screen mode."};

inline constexpr Option kEnemyDamage{  // battle/battle_rates
    L"Battle", L"enemy-damage", kTenthsWords, L"1.0",
    L"the damage enemies deal to the players' party, times this (0.1 to 10.0, steps of 0.1; 1.0 is the game's); from the "
    L"next hit, with the game's own rounding and its 9999 cap",
    false, L"Enemy Damage", kTenthsLabels,
    L"Multiplier controlling the damage received from regular enemies and bosses."};
inline constexpr Option kEnemyHp{  // battle/battle_rates
    L"Battle", L"enemy-hp", kTenthsWords, L"1.0",
    L"every enemy's max HP but the bosses', times this (0.1 to 10.0); from the next fight",
    false, L"Enemy HP", kTenthsLabels,
    L"Multiplier controlling the health of regular enemies."};
inline constexpr Option kBossHp{  // battle/battle_rates
    L"Battle", L"boss-hp", kTenthsWords, L"1.0",
    L"the max HP of the enemies the game marks as a fight's leader (bosses, summon bosses, named foes), times this "
    L"(0.1 to 10.0); from the next fight",
    false, L"Boss HP", kTenthsLabels,
    L"Multiplier controlling the health of bosses."};
inline constexpr Option kEnemyStagger{  // battle/battle_rates
    L"Battle", L"enemy-stagger", kTenthsWords, L"1.0",
    L"the size of every enemy's stagger gauge, times this (0.1 to 10.0; higher staggers later); from the next fight",
    false, L"Enemy Stagger Gauge", kTenthsLabels,
    L"Multiplier controlling the stagger gauge of regular enemies and bosses."};
inline constexpr Option kExpGain{  // battle/battle_rates
    L"Battle", L"exp-gain", kTenthsWords, L"1.0",
    L"the EXP a won battle gives, times this (0.1 to 10.0), on top of the game's own bonus after the story or with "
    L"2x EXP & 3x AP on; through the game's own bonus rate",
    false, L"EXP Gained", kTenthsLabels,
    L"Multiplier controlling the EXP earned from battle encounters. Adds to the game's own 2x EXP option."};
inline constexpr Option kApGain{  // battle/battle_rates
    L"Battle", L"ap-gain", kTenthsWords, L"1.0",
    L"the AP a won battle gives, times this (0.1 to 10.0), on top of the game's own bonus after the story or with "
    L"2x EXP & 3x AP on; through the game's own bonus rate",
    false, L"AP Gained", kTenthsLabels,
    L"Multiplier controlling the AP earned from battle encounters. Adds to the game's own 3x AP option."};

inline constexpr std::array<const wchar_t*, 2> kLeadHolderWords{L"guide", L"keeps"};
inline constexpr std::array<const wchar_t*, 2> kLeadHolderLabels{L"AI-controlled", L"Player-controlled"};
inline constexpr Option kLeadHolder{  // players/lead_gate
    L"Experimental", L"lead-holder", kLeadHolderWords, L"keeps",
    L"a lead or run to a mark the level needs finished, on player 2 or 3's character: guide (to its AI for the whole "
    L"walk, the player's view on it) or keeps (the default: the player keeps it, the stop marked in that player's view, "
    L"its AI finishing the arrival within 2 m of the stop)",
    true, L"Scripted Sequences", kLeadHolderLabels,
    L"Determines whether additional players get to control their selected characters during certain scripted segments.\r\n\r\n"
    L"AI-controlled: The game takes over and automatically drives the party members as required.\r\nPlayer-controlled: the "
    L"player directs the party member to marked locations."};
inline constexpr Option kAreaLevels{  // world/area_levels
    L"Experimental", L"area-levels", kAreaLevelsWords, L"on",
    L"on: every player's area stays loaded and solid, not only player 1's, at the cost of more memory (player 2 and "
    L"3's areas stay loaded beside player 1's); off: the world follows player 1's area alone; from the next request",
    true, L"World Loading", kAreaLevelsLabels,
    L"Only Player 1: the visible world loads only around Player 1's immediate area.\r\nAll Players: the world loads around "
    L"every player's position, preventing the world from unloading when no longer in range of Player 1. Uses more "
    L"memory."};

inline constexpr Option kTalkGather{  // world/talk_gather
    L"Experimental", L"talk-gather", kOnOffWords, L"on",
    L"a talk another player starts more than 8 m from player 1's character: on (the default: player 1's character is "
    L"brought beside that player, facing the one they talk to) or off (it stays where it stands and the talk plays with "
    L"it there)",
    true, L"Gather Player 1 for Conversations", kOnOffLabels,
    L"Whether another player speaking to an NPC gathers Player 1 from their current location."};
inline constexpr Option kTalkFollow{  // world/talk_gather, players/session
    L"Experimental", L"talk-follow", kOnOffWords, L"on",
    L"a talk player 1 starts: on (the default: the other players' characters go to their AI for the talk and walk over to "
    L"player 1's, as companions do in single player) or off (they stay with their players and stand where they are until "
    L"the talk ends; one the talk itself uses is taken for its lines and comes back where it stood); either way every "
    L"view shows the talk's lines, and its camera when it has one",
    true, L"Gather Party for Conversations", kOnOffLabels,
    L"Whether Player 1 speaking to an NPC gathers other players from their current location."};

// Players 2 and 3's camera responsiveness (players/seat_responsiveness): the game's own Responsiveness levels, 1 to 5,
// player 1's being the game's option.
inline constexpr std::array<const wchar_t*, 5> kResponsivenessWords{L"1", L"2", L"3", L"4", L"5"};
inline constexpr Option kResponsivenessTwo{  // players/seat_responsiveness
    L"Camera", L"responsiveness.player-2", kResponsivenessWords, L"3",
    L"player 2's camera responsiveness, as the game's own Responsiveness option sets player 1's: 1 (the slowest turn) to 5 "
    L"(the fastest); from player 2's next frame",
    false, L"Responsiveness (Player 2)", kResponsivenessWords,
    L"Set camera responsiveness for Player 2. The higher the number, the more responsive the camera will be.",
    false, false, true};
inline constexpr Option kResponsivenessThree{  // players/seat_responsiveness
    L"Camera", L"responsiveness.player-3", kResponsivenessWords, L"3",
    L"player 3's camera responsiveness, as the game's own Responsiveness option sets player 1's: 1 (the slowest turn) to 5 "
    L"(the fastest); from player 3's next frame",
    false, L"Responsiveness (Player 3)", kResponsivenessWords,
    L"Set camera responsiveness for Player 3. The higher the number, the more responsive the camera will be.",
    false, false, true};

inline constexpr Option kConsole{  // mod.cpp, control/remote
    L"Development", L"console", kOnOffWords, L"off",
    L"the development console on 127.0.0.1:4418, for tools/game.py: on opens it at the next start; off (the default) opens no "
    L"port",
    false, L"Development Console", kOnOffLabels,
    L"Opens the mod's development console at the next start, reachable from this computer only (127.0.0.1:4418), for the "
    L"repository's tools/game.py. Off by default.",
    false, true};

/// Every option, in the order the settings file lists them.
inline constexpr std::array kOptions{&kMultiMonitor, &kMonitors,    &kColumnOrder,  &kTwoPlayerSplit, &kThreePlayerSplit, &kHorizontalFov,
                                     &kVerticalFov,  &kHudScale,    &kHudScaleTwo,  &kHudScaleThree,  &kMenuSlow,         &kBattleWarps,
                                     &kEnemyDamage,  &kEnemyHp,     &kBossHp,
                                     &kEnemyStagger, &kExpGain,     &kApGain,       &kLeadHolder,     &kAreaLevels,       &kTalkGather,
                                     &kTalkFollow,   &kResponsivenessTwo, &kResponsivenessThree, &kConsole};

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
                  Listed(kHorizontalFov) && Listed(kVerticalFov) && Listed(kHudScale) && Listed(kHudScaleTwo) && Listed(kHudScaleThree) && Listed(kMenuSlow) && Listed(kBattleWarps) && Listed(kEnemyDamage) && Listed(kEnemyHp) && Listed(kBossHp) &&
                  Listed(kEnemyStagger) && Listed(kExpGain) && Listed(kApGain) && Listed(kLeadHolder) && Listed(kAreaLevels) &&
                  Listed(kTalkGather) && Listed(kTalkFollow) && Listed(kResponsivenessTwo) && Listed(kResponsivenessThree) &&
                  Listed(kConsole),
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
