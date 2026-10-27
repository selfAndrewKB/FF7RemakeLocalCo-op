#include "views/whole_screen.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/field_root.h"
#include "players/game_state.h"
#include "views/scene_camera.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace lss::views::whole_screen {

namespace {

// EndFieldOnOffTable (Dumps/CXXHeaderDump/EndGame_enums.hpp; research/RE-lifecycle-world.md §2.4: the ids the game
// state's own menu opener reads): ShowMainMenu, ShowShopMenu, ShowMapJournal, ShowTutorial, ShowTutorial_TouchPanel.
// 78 ShowColosseumMenu is not a menu: it is held for a whole VR simulator or colosseum session, fights included
// (research/RE-minigame-signal.md §1), so it is the arena signal, read with the minigame bit and the battle state.
constexpr std::array<std::uint32_t, 5> kMenuIds{72, 73, 74, 75, 76};
constexpr std::uint32_t                kShowColosseumMenu = 78;
constexpr std::uint32_t                kMiniGamePlaying = 36;
constexpr std::uint32_t                kShowSkipPauseMenu = 12;
constexpr std::array<std::uint32_t, 3> kBlackIds{39, 40, 53};  // Game_Blackout, Level_Blackout, ContinueEntryLocationBlackout
constexpr std::uint32_t                kShowLoadingScreen = 41;

// Set at install: the addresses holding the shared bike state and its reference block (0: not read).
std::uintptr_t g_bikeShared  = 0;
std::uintptr_t g_bikeRefCtrl = 0;

/// The game's own "a bike chase runs" test (facts::kBikeGameShared).
bool BikeChase() noexcept
{
    std::uintptr_t shared = 0, ctrl = 0;
    std::int32_t   count  = 0;
    return g_bikeShared != 0 && platform::Read(reinterpret_cast<const void*>(g_bikeShared), shared) && shared != 0 &&
           platform::Read(reinterpret_cast<const void*>(g_bikeRefCtrl), ctrl) && ctrl != 0 &&
           platform::Read(reinterpret_cast<const void*>(ctrl + facts::kBikeGameSharedRefCount), count) && count > 0;
}

// Game thread.
Signals g_signals;
Reason  g_reason = Reason::None;
bool    g_logged = false;

Signals Read() noexcept
{
    Signals s;
    const players::game_state::Snapshot& state = players::game_state::Current();
    s.readable = state.valid;
    if (!s.readable)
    {
        return s;
    }
    s.gameState = state.state;
    s.menuKind  = state.menu;
    s.title     = IsTitle(state.location);
    for (const std::uint32_t id : kMenuIds)
    {
        s.scriptMenu = s.scriptMenu || players::field_root::OnOff(id);
    }
    s.minigame    = players::field_root::OnOff(kMiniGamePlaying);
    s.arena       = players::field_root::OnOff(kShowColosseumMenu);
    s.inBattle    = players::field_root::InBattle();
    s.skippable   = players::field_root::OnOff(kShowSkipPauseMenu) && !s.arena;  // the VR act holds 12 for its session
    for (const std::uint32_t id : kBlackIds)
    {
        s.black = s.black || players::field_root::OnOff(id);
    }
    s.loading     = players::field_root::OnOff(kShowLoadingScreen);
    s.bike        = BikeChase();
    s.shopUp      = players::game_state::ShopUp();
    s.pauseWidget = players::game_state::PauseWidgetAlive();
    // The cutscene manager and the field control lock come through the scene camera's read (Scene{} while its
    // globals failed their check: no cutscene, skip or scene hold is known then). The lock holds the whole screen
    // outside a battle only (the battle AI and the boss restrictions write lock bits of their own,
    // research/RE-minigame-signal.md §1-2: a fight never collapses), and without the arena's bit 21, which a VR or
    // colosseum session switches by phase: its menus collapse by ArenaMenu, its fights stay split.
    const scene_camera::Scene scene = scene_camera::ReadSceneNow();
    s.cutscene    = scene.playing;
    s.skipping    = scene.skip;
    s.minigameBit = (scene.lockMask & facts::kControlLockMiniGame) != 0;
    const std::uint32_t held = scene.lockMask & ~scene.mover & ~(s.arena ? facts::kControlLockMiniGame : 0u);
    s.sceneHolds = !s.inBattle && (scene.playing || held != 0 || scene.lockCount != 0);
    return s;
}

std::wstring SignalsText(const Signals& s)
{
    if (!s.readable)
    {
        return L"the game state is not readable";
    }
    return std::format(L"game state {} ({}, menu kind {}), script menu {}, shop {}, pause widget {}, arena {} (minigame bit {}, "
                       L"in battle {}), minigame {}, cutscene {}, bike {}, scene holds {}, skippable {}, skip {}, black {}, "
                       L"loading {}, title {}",
                       players::game_state::StateName(s.gameState), s.gameState, s.menuKind, s.scriptMenu ? L"on" : L"off",
                       s.shopUp ? L"up" : L"down", s.pauseWidget ? L"alive" : L"none", s.arena ? L"on" : L"off",
                       s.minigameBit ? L"on" : L"off", s.inBattle ? L"yes" : L"no", s.minigame ? L"on" : L"off",
                       s.cutscene ? L"playing" : L"no", s.bike ? L"yes" : L"no", s.sceneHolds ? L"yes" : L"no", s.skippable ? L"on" : L"off",
                       s.skipping ? L"on" : L"off", s.black ? L"on" : L"off", s.loading ? L"on" : L"off", s.title ? L"yes" : L"no");
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto shared = image.Rebase(facts::kBikeGameShared, sizeof(std::uintptr_t));
    const auto ctrl   = image.Rebase(facts::kBikeGameSharedRefCtrl, sizeof(std::uintptr_t));
    if (shared && ctrl)
    {
        g_bikeShared  = *shared;
        g_bikeRefCtrl = *ctrl;
    }
    else
    {
        platform::Log(L"whole screen: the bike state's globals are outside the image; a bike chase is not read");
    }
    control::Register({.name  = L"whole-screen",
                       .shape = control::Shape::Text,
                       .help  = L"what takes the whole screen now (the reason the one-window single view collapses the split), "
                                L"and the game's signals it is read from",
                       .get   = [] { return control::Value{std::format(L"{}: {}", NameOf(g_reason), SignalsText(g_signals))}; }});
    platform::Log(L"whole screen: menus, shops, scenes, a cutscene skip, a held black screen, the loading screen and the title "
                  L"are read from the game's own state each tick (the one-window single view's reasons); minigames by on/off 36, the "
                  L"bike chase by its shared state, the VR simulator's and colosseum's menus by on/off 78 with the minigame bit "
                  L"outside a battle (research/RE-minigame-signal.md)");
    return true;
}

void Tick() noexcept
{
    const Signals now    = Read();
    const Reason  reason = ReasonOf(now);
    // Info when something takes the whole screen or lets go (the state change); a reason changing within one moment
    // (a cutscene into a held black into a loading screen) and each change of the signals at Debug.
    if (!g_logged || (reason != Reason::None) != (g_reason != Reason::None))
    {
        platform::Log(L"whole screen: {}", NameOf(reason));
    }
    else if (reason != g_reason)
    {
        platform::LogDebug(L"whole screen: {} (within the moment)", NameOf(reason));
    }
    if ((!g_logged || !(now == g_signals)) && platform::Enabled(platform::Level::Debug))
    {
        try
        {
            platform::LogDebug(L"whole screen: {} -> {}", SignalsText(now), NameOf(reason));
        }
        catch (...)
        {
        }
    }
    g_signals = now;
    g_reason  = reason;
    g_logged  = true;
}

Reason ReasonNow() noexcept
{
    return g_reason;
}

bool Shown() noexcept
{
    return g_reason != Reason::None;
}

Signals SignalsNow() noexcept
{
    return g_signals;
}

}  // namespace lss::views::whole_screen
