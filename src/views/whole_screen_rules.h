#pragma once

// Whether the game shows something that takes the whole screen: the decision, pure (host-tested,
// tests/whole_screen_rules_test.cpp), over the signals views/whole_screen.cpp reads each tick (views/whole_screen.h
// names the game state behind each one). The reason is what the one-window single view collapses the split for
// (players/single_view.h; views/column_source.h Why::WholeScreen). Rebirth's views/whole_screen_reason.h gave the
// shape; the signals and their order are Remake's (research/RE-lifecycle-world.md §8).

#include <cstdint>
#include <string_view>

namespace lss::views::whole_screen {

/// The game's signals, as read this tick.
struct Signals
{
    bool         readable  = false;  // the game state was read this tick (false: nothing below is known)
    std::uint8_t gameState = 0;      // EGameState (players/game_state.h)
    std::uint8_t menuKind  = 0;      // the pause menu's kind (EEndSystemPauseMenuKind), for the log
    bool scriptMenu  = false;  // an on/off menu id is on: 72 main menu, 73 shop, 74 map journal, 75/76 tutorial
    bool arena       = false;  // on/off 78 ShowColosseumMenu: a VR simulator or colosseum session, menus and fights alike
    bool minigameBit = false;  // the controller-exclusion bit 21 (DisableController_MiniGame) of the field lock mask
    bool inBattle    = false;  // the field battle state says a battle is on
    bool shopUp      = false;  // the shop UI's own "a shop is up" byte (a shop runs in the field state)
    bool pauseWidget = false;  // the game's IsPauseMenuClosed says a pause-family menu is alive
    bool cutscene    = false;  // the cutscene manager plays a scene
    bool sceneHolds  = false;  // a scene holds every player: the cutscene, or the field control lock outside player 1's own
                               // move, read outside a battle and without the arena's bit 21 (views/whole_screen.cpp)
    bool skippable   = false;  // on/off 12 ShowSkipPauseMenu: a skippable scene
    bool skipping    = false;  // the cutscene manager's skip byte (the scene fast-forwards under black)
    bool black       = false;  // on/off 39, 40 or 53: a held black screen
    bool loading     = false;  // on/off 41: the loading screen
    bool minigame    = false;  // on/off 36 MiniGamePlaying (Fort Condor natively; which act rows set it: research/RE-minigame-signal.md)
    bool bike        = false;  // a bike chase: the game's own test of its shared bike state
    bool title       = false;  // the field root's location is the title's

    bool operator==(const Signals&) const noexcept = default;  // every field (the log's change test)
};

/// The first reason that holds, in this order.
enum class Reason : std::uint8_t
{
    None,
    PauseMenu,       // EGameState 8: a pause menu owns the screen (main menu, map, battle pause, game over, colosseum)
    CinemaPause,     // EGameState 7: the pause during a cutscene
    ChapterPause,    // EGameState 10: the pause during a chapter start
    ScriptMenu,      // an on/off menu id (a script-opened menu, shop, tutorial)
    Shop,            // the shop UI's own byte
    PauseWidget,     // a pause-family widget alive (a Blueprint-opened pause menu, Libra from the main menu)
    ArenaMenu,       // a VR simulator or colosseum session outside its fights (78 with bit 21, not in battle): its menus
    Minigame,        // on/off 36
    Bike,            // a bike chase (chapters 4 and 18)
    Cutscene,        // the cutscene manager playing
    Scene,           // a scene holds every player (a talk, an event camera: the field control lock)
    SkippableScene,  // on/off 12
    CutsceneSkip,    // the skip byte
    Black,           // a held black screen
    Loading,         // the loading screen
    Title,
};

/// EGameState's pause states (research/RE-lifecycle-world.md §2.2).
inline constexpr std::uint8_t kStateCinemaPause  = 7;
inline constexpr std::uint8_t kStatePauseMenu    = 8;
inline constexpr std::uint8_t kStateChapterPause = 10;

/// The title's location name as the field root carries it (players/game_state.h `game.location`; live 2026-10-18).
inline constexpr std::wstring_view kTitleLocation = L"005-TITLB";

constexpr bool IsTitle(std::wstring_view location) noexcept
{
    return location == kTitleLocation;
}

constexpr Reason ReasonOf(const Signals& s) noexcept
{
    if (!s.readable)
    {
        return Reason::None;
    }
    if (s.gameState == kStatePauseMenu)
    {
        return Reason::PauseMenu;
    }
    if (s.gameState == kStateCinemaPause)
    {
        return Reason::CinemaPause;
    }
    if (s.gameState == kStateChapterPause)
    {
        return Reason::ChapterPause;
    }
    if (s.scriptMenu)
    {
        return Reason::ScriptMenu;
    }
    if (s.shopUp)
    {
        return Reason::Shop;
    }
    if (s.pauseWidget)
    {
        return Reason::PauseWidget;
    }
    if (s.arena && s.minigameBit && !s.inBattle)
    {
        return Reason::ArenaMenu;
    }
    if (s.minigame)
    {
        return Reason::Minigame;
    }
    if (s.bike)
    {
        return Reason::Bike;
    }
    if (s.cutscene)
    {
        return Reason::Cutscene;
    }
    if (s.sceneHolds)
    {
        return Reason::Scene;
    }
    if (s.skippable)
    {
        return Reason::SkippableScene;
    }
    if (s.skipping)
    {
        return Reason::CutsceneSkip;
    }
    if (s.black)
    {
        return Reason::Black;
    }
    if (s.loading)
    {
        return Reason::Loading;
    }
    return s.title ? Reason::Title : Reason::None;
}

/// For the log.
constexpr const wchar_t* NameOf(Reason reason) noexcept
{
    switch (reason)
    {
        case Reason::None: return L"none";
        case Reason::PauseMenu: return L"a pause menu (game state 8)";
        case Reason::CinemaPause: return L"the cutscene pause (game state 7)";
        case Reason::ChapterPause: return L"the chapter-start pause (game state 10)";
        case Reason::ScriptMenu: return L"a script-opened menu (the on/off table)";
        case Reason::Shop: return L"a shop";
        case Reason::PauseWidget: return L"a pause-family menu (IsPauseMenuClosed)";
        case Reason::ArenaMenu: return L"a VR simulator or colosseum menu (on/off 78 with the minigame bit, not in battle)";
        case Reason::Minigame: return L"a minigame (on/off 36)";
        case Reason::Bike: return L"a bike chase";
        case Reason::Cutscene: return L"a cutscene";
        case Reason::Scene: return L"a scene holding the players (a talk, an event camera)";
        case Reason::SkippableScene: return L"a skippable scene (on/off 12)";
        case Reason::CutsceneSkip: return L"a cutscene skip";
        case Reason::Black: return L"a held black screen";
        case Reason::Loading: return L"the loading screen";
        case Reason::Title: return L"the title";
    }
    return L"?";
}

}  // namespace lss::views::whole_screen
