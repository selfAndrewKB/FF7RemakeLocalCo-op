#include "views/whole_screen.h"

#include "battle/sequence_camera.h"
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

// EndFieldOnOffTable (Dumps/CXXHeaderDump/EndGame_enums.hpp; research/RE-lifecycle-world.md: the ids the game
// state's own menu opener reads): ShowMainMenu, ShowShopMenu, ShowMapJournal, ShowTutorial, ShowTutorial_TouchPanel.
// 78 ShowColosseumMenu is not a menu: it is held for a whole VR simulator or colosseum session, fights included
// (research/RE-minigame-signal.md), so it is the arena signal, read with the minigame bit and the battle state.
constexpr std::array<std::uint32_t, 5> kMenuIds{72, 73, 74, 75, 76};
constexpr std::uint32_t                kShowColosseumMenu = 78;
constexpr std::uint32_t                kMiniGamePlaying = 36;
constexpr std::uint32_t                kShowSkipPauseMenu = 12;
constexpr std::array<std::uint32_t, 3> kBlackIds{39, 40, 53};  // Game_Blackout, Level_Blackout, ContinueEntryLocationBlackout
constexpr std::uint32_t                kShowLoadingScreen = 41;

// Set at install: the addresses holding the shared bike state and its reference block, and the summon manager's
// pointer (0: not read).
std::uintptr_t g_bikeShared     = 0;
std::uintptr_t g_bikeRefCtrl    = 0;
std::uintptr_t g_summonManager  = 0;

/// The summon manager's state and event lock into `s` (both 0 without a manager).
void ReadSummon(Signals& s) noexcept
{
    std::uintptr_t manager = 0;
    if (g_summonManager == 0 || !platform::Read(reinterpret_cast<const void*>(g_summonManager), manager) || manager == 0 ||
        !platform::Read(reinterpret_cast<const void*>(manager + facts::kSummonManagerState), s.summonState) ||
        !platform::Read(reinterpret_cast<const void*>(manager + facts::kSummonManagerEvent), s.summonEvent))
    {
        s.summonState = 0;
        s.summonEvent = 0;
    }
}

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
Signals  g_signals;
Reason   g_reason   = Reason::None;
unsigned g_offTicks = kMinigameGapTicks + 1;  // ticks since this tick's own reason was last Minigame (the act-gap bridge; capped)
bool     g_logged   = false;

Signals Read(const void* managerOne) noexcept
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
    s.minigameAct = s.minigame ? players::field_root::OnOffOwner(kMiniGamePlaying) : 0;
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
    // A summon's introduction: the summon manager's own hold of the call (its event lock), a battle camera sequence the
    // game plays as one moment for the whole party; the summon's fight after it stays split.
    ReadSummon(s);
    // The battle camera shot the operators applied last tick, by the kind the game gave it: a scene-like one (a phase
    // change, a summon's entry or special, a boss's finish) plays on every player's camera as one scene.
    s.shotMode = battle::sequence_camera::AppliedShotMode().value_or(kShotModeNone);
    // The cutscene manager and the field control lock's mask come through the scene camera's read (Scene{} while its
    // globals failed their check: no cutscene, skip, bit or held scene is known then, and so no event camera reason).
    // The lock itself is no reason: a free talk or a pickup holds it, and those play on every view;
    // a scripted talk's event camera is its own signal below, which the lock gates; its bit 21 is read
    // for the arena rule (a VR or colosseum session switches it by phase: its menus collapse by ArenaMenu, its fights
    // stay split).
    const scene_camera::Scene scene = scene_camera::ReadSceneNow();
    s.cutscene    = scene.playing;
    s.skipping    = scene.skip;
    s.minigameBit = (scene.lockMask & facts::kControlLockMiniGame) != 0;
    // The event camera system's slot on player 1's manager: a scripted talk (the game's own angles and cuts, the HUD
    // off, the speakers alone in frame) or an event; a free talk keeps the field camera's slot (seen live:
    // Chadley's scripted talk on 18 with cuts, the armed leader's free talk on 24 alone). A field area's rail camera
    // writes 18 in free play too (research/RE-cutscene-camera-two.md), so the rule takes the slot only
    // while a scene holds the players (the lock outside player 1's own move, or the cutscene) [ASSUMPTION until a
    // rail area is played: a rail camera holds no lock].
    s.eventCamera = scene_camera::SlotActive(managerOne, facts::kCameraSlotEvent);
    s.sceneHeld   = scene.HoldsAnyone();
    return s;
}

std::wstring SignalsText(const Signals& s)
{
    if (!s.readable)
    {
        return L"the game state is not readable";
    }
    return std::format(L"game state {} ({}, menu kind {}), script menu {}, shop {}, pause widget {}, arena {} (minigame bit {}, "
                       L"in battle {}), minigame {} (by {}), cutscene {}, event camera {} (scene held {}), bike {}, skippable {}, skip {}, "
                       L"black {}, loading {}, title {}, summon state {} (event {}), battle shot mode {}",
                       players::game_state::StateName(s.gameState), s.gameState, s.menuKind, s.scriptMenu ? L"on" : L"off",
                       s.shopUp ? L"up" : L"down", s.pauseWidget ? L"alive" : L"none", s.arena ? L"on" : L"off",
                       s.minigameBit ? L"on" : L"off", s.inBattle ? L"yes" : L"no", s.minigame ? L"on" : L"off",
                       players::field_root::OnOffOwnerText(s.minigameAct), s.cutscene ? L"playing" : L"no",
                       s.eventCamera ? L"on" : L"off", s.sceneHeld ? L"yes" : L"no", s.bike ? L"yes" : L"no",
                       s.skippable ? L"on" : L"off", s.skipping ? L"on" : L"off", s.black ? L"on" : L"off",
                       s.loading ? L"on" : L"off", s.title ? L"yes" : L"no", s.summonState, s.summonEvent,
                       s.shotMode == kShotModeNone ? std::wstring(L"none") : std::to_wstring(s.shotMode));
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
    if (const auto summon = image.Rebase(facts::kSummonManager, sizeof(std::uintptr_t)))
    {
        g_summonManager = *summon;
    }
    else
    {
        platform::Log(L"whole screen: the summon manager's global is outside the image; a summon's introduction stays split");
    }
    control::Register({.name  = L"whole-screen",
                       .shape = control::Shape::Text,
                       .help  = L"what takes the whole screen now (the reason the one-window single view collapses the split), "
                                L"and the game's signals it is read from",
                       .get   = [] {
                           return control::Value{std::format(L"{}: {}{}", NameOf(g_reason), SignalsText(g_signals),
                                                             ReasonOf(g_signals) != g_reason ? L" (held over the act gap)" : L"")};
                       }});
    platform::Log(L"whole screen: menus, shops, cutscenes, the event camera (a scripted talk, an event: slot 18 on player 1's "
                  L"camera manager while the field lock or a cutscene holds the players, outside a battle), a cutscene skip, a "
                  L"held black screen, the loading screen and the title are read "
                  L"from the game's own state each tick (the one-window single view's reasons; a free talk or a pickup holding the "
                  L"field lock alone is none); a summon's introduction by the summon manager's call; a scene-like battle shot "
                  L"(a phase change, a summon's entry or special, a boss's finish) by the kind the game gives it; "
                  L"minigames by on/off 36 (held up to {} ticks over the gap between a segment's "
                  L"chained acts), the bike chase by its shared state, the VR simulator's and colosseum's menus by on/off 78 with the "
                  L"minigame bit outside a battle (research/RE-minigame-signal.md)",
                  kMinigameGapTicks);
    return true;
}

void Tick(const void* managerOne) noexcept
{
    const Signals now = Read(managerOne);
    const Reason  raw = ReasonOf(now);
    g_offTicks          = NextTicksOff(raw, g_offTicks);
    const Reason reason = BridgeActGap(raw, g_reason, g_offTicks);
    // Info when something takes the whole screen or lets go (the state change); a reason changing within one moment
    // (a cutscene into a held black into a loading screen), the bridge over an act gap, and each change of the
    // signals at Debug.
    if (!g_logged || (reason != Reason::None) != (g_reason != Reason::None))
    {
        platform::Log(L"whole screen: {}", NameOf(reason));
    }
    else if (reason != g_reason)
    {
        platform::LogDebug(L"whole screen: {} (within the moment)", NameOf(reason));
    }
    if (reason != raw && g_offTicks == 1)
    {
        platform::LogDebug(L"whole screen: on/off 36 went off; the minigame moment is held up to {} ticks for its next act", kMinigameGapTicks);
    }
    if ((!g_logged || !(now == g_signals)) && platform::Enabled(platform::Level::Debug))
    {
        try
        {
            platform::LogDebug(L"whole screen: {} -> {}{}", SignalsText(now), NameOf(reason), reason != raw ? L" (held over the act gap)" : L"");
        }
        catch (...)
        {
        }
    }
    g_signals = now;
    g_reason  = reason;
    g_logged  = true;
}

bool Shown() noexcept
{
    return g_reason != Reason::None;
}

Reason Current() noexcept
{
    return g_reason;
}

}  // namespace lss::views::whole_screen
