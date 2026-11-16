// Host-side tests for views/whole_screen_rules.h: which game signals take the whole screen (the one-window single
// view's reasons), in their order.

#include "views/whole_screen_rules.h"

#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "whole_screen_rules: all passed\n" : "whole_screen_rules: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
}  // namespace lss::test

#define EXPECT(condition)                                                \
    do                                                                   \
    {                                                                    \
        if (!(condition))                                                \
        {                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition); \
            ++lss::test::g_failures;                                     \
        }                                                                \
    } while (false)

namespace {

using namespace lss::views::whole_screen;

constexpr Signals Readable()
{
    Signals s;
    s.readable  = true;
    s.gameState = 6;  // play
    return s;
}

/// An unreadable game state gives no reason, whatever else is set; the field running gives none.
void NothingKnown()
{
    Signals s;
    s.gameState = kStatePauseMenu;
    s.cutscene  = true;
    s.title     = true;
    EXPECT(ReasonOf(s) == Reason::None);
    EXPECT(ReasonOf(Readable()) == Reason::None);
    static_assert(ReasonOf(Readable()) == Reason::None, "constexpr");
}

/// Each signal alone gives its reason.
void EachSignal()
{
    auto s      = Readable();
    s.gameState = kStatePauseMenu;
    EXPECT(ReasonOf(s) == Reason::PauseMenu);
    s.gameState = kStateCinemaPause;
    EXPECT(ReasonOf(s) == Reason::CinemaPause);
    s.gameState = kStateChapterPause;
    EXPECT(ReasonOf(s) == Reason::ChapterPause);
    s            = Readable();
    s.scriptMenu = true;
    EXPECT(ReasonOf(s) == Reason::ScriptMenu);
    s        = Readable();
    s.shopUp = true;
    EXPECT(ReasonOf(s) == Reason::Shop);
    s             = Readable();
    s.pauseWidget = true;
    EXPECT(ReasonOf(s) == Reason::PauseWidget);
    s             = Readable();
    s.arena       = true;
    s.minigameBit = true;
    EXPECT(ReasonOf(s) == Reason::ArenaMenu);
    s          = Readable();
    s.minigame = true;
    EXPECT(ReasonOf(s) == Reason::Minigame);
    s      = Readable();
    s.bike = true;
    EXPECT(ReasonOf(s) == Reason::Bike);
    // A summon: its call (state 5) and its introduction (present with the call's event lock) take the screen; its fight
    // (present, no lock) and its departure (lock 2, the game's own cutscene) do not by this signal.
    s             = Readable();
    s.summonState = kSummonStateCall;
    EXPECT(ReasonOf(s) == Reason::SummonCall);
    s.summonState = kSummonStatePresent;
    s.summonEvent = kSummonEventCall;
    EXPECT(ReasonOf(s) == Reason::SummonCall);
    s.summonEvent = 0;
    EXPECT(ReasonOf(s) == Reason::None);
    s.summonEvent = 2;
    EXPECT(ReasonOf(s) == Reason::None);
    s.summonState = 0;
    s.summonEvent = kSummonEventCall;  // a reset that zeroes the state but leaves the lock: not a summon's call
    EXPECT(ReasonOf(s) == Reason::None);
    s          = Readable();
    s.cutscene = true;
    EXPECT(ReasonOf(s) == Reason::Cutscene);
    s             = Readable();
    s.eventCamera = true;  // a rail camera in free play: nothing holds the players, no reason
    EXPECT(ReasonOf(s) == Reason::None);
    s.sceneHeld = true;  // a scripted talk: the lock and the event camera together
    EXPECT(ReasonOf(s) == Reason::EventCamera);
    s.inBattle = true;  // if a fight's camera ever takes 18: the fight stays split
    EXPECT(ReasonOf(s) == Reason::None);
    s           = Readable();
    s.sceneHeld = true;  // a free talk or a pickup: the lock alone
    EXPECT(ReasonOf(s) == Reason::None);
    s           = Readable();
    s.skippable = true;
    EXPECT(ReasonOf(s) == Reason::SkippableScene);
    s          = Readable();
    s.skipping = true;
    EXPECT(ReasonOf(s) == Reason::CutsceneSkip);
    s       = Readable();
    s.black = true;
    EXPECT(ReasonOf(s) == Reason::Black);
    s         = Readable();
    s.loading = true;
    EXPECT(ReasonOf(s) == Reason::Loading);
    s       = Readable();
    s.title = true;
    EXPECT(ReasonOf(s) == Reason::Title);
}

/// The order (which reason the log names; Shown() is "not None" either way): a pause state over a menu and a scene,
/// a menu over a cutscene, a cutscene over its skip prompt, a chase over its opening scene, a cutscene and a minigame
/// over an event camera.
void Order()
{
    // A summon's call: under a battle pause the pause menu; over a cutscene flag the call; in a battle still the call.
    auto summon        = Readable();
    summon.summonState = kSummonStateCall;
    summon.inBattle    = true;
    EXPECT(ReasonOf(summon) == Reason::SummonCall);
    summon.cutscene = true;
    EXPECT(ReasonOf(summon) == Reason::SummonCall);
    summon.gameState = kStatePauseMenu;
    EXPECT(ReasonOf(summon) == Reason::PauseMenu);
    auto s      = Readable();
    s.gameState = kStateCinemaPause;
    s.cutscene  = true;
    EXPECT(ReasonOf(s) == Reason::CinemaPause);
    s            = Readable();
    s.gameState  = kStatePauseMenu;
    s.shopUp     = true;  // a shop re-opened under a pause menu
    s.scriptMenu = true;
    EXPECT(ReasonOf(s) == Reason::PauseMenu);
    s          = Readable();
    s.shopUp   = true;
    s.cutscene = true;  // a shop opened by a scene
    EXPECT(ReasonOf(s) == Reason::Shop);
    s           = Readable();
    s.cutscene  = true;
    s.skippable = true;
    EXPECT(ReasonOf(s) == Reason::Cutscene);
    s             = Readable();
    s.cutscene    = true;
    s.sceneHeld   = true;
    s.eventCamera = true;  // a cutscene starting over an event camera (the 18 -> 8 blend)
    EXPECT(ReasonOf(s) == Reason::Cutscene);
    s             = Readable();
    s.minigame    = true;
    s.sceneHeld   = true;
    s.eventCamera = true;  // the Reactor 5 segment's phases run on the event camera too
    EXPECT(ReasonOf(s) == Reason::Minigame);
    s          = Readable();
    s.bike     = true;
    s.cutscene = true;  // a chase's opening scene: the chase holds through it
    EXPECT(ReasonOf(s) == Reason::Bike);
    // A VR or colosseum session: its fight stays split (78 with the bit in a battle), its menu collapses, a pause over
    // it is the pause's.
    s             = Readable();
    s.arena       = true;
    s.minigameBit = true;
    s.inBattle    = true;
    EXPECT(ReasonOf(s) == Reason::None);
    s.cutscene = true;  // a cutscene inside the fight is still a cutscene
    EXPECT(ReasonOf(s) == Reason::Cutscene);
    s.cutscene = false;
    s.inBattle = false;
    EXPECT(ReasonOf(s) == Reason::ArenaMenu);
    s.gameState = kStatePauseMenu;
    EXPECT(ReasonOf(s) == Reason::PauseMenu);
    s           = Readable();
    s.arena     = true;  // 78 alone (the bit off, a fight or a phase between): nothing
    s.inBattle  = true;
    EXPECT(ReasonOf(s) == Reason::None);
    s          = Readable();
    s.skipping = true;
    s.black    = true;
    EXPECT(ReasonOf(s) == Reason::CutsceneSkip);
    s.readable = false;
    EXPECT(ReasonOf(s) == Reason::None);
}

/// The bridge over the gap between a minigame segment's chained acts: a minigame moment holds while nothing takes the
/// screen and on/off 36 has been off for at most kMinigameGapTicks ticks; a longer gap ends it; any other reason is
/// its own; no other moment is bridged.
void ActGap()
{
    static_assert(kMinigameGapTicks >= 1, "the gap seen was one tick");
    EXPECT(BridgeActGap(Reason::None, Reason::Minigame, 1) == Reason::Minigame);
    EXPECT(BridgeActGap(Reason::None, Reason::Minigame, kMinigameGapTicks) == Reason::Minigame);
    EXPECT(BridgeActGap(Reason::None, Reason::Minigame, kMinigameGapTicks + 1) == Reason::None);
    EXPECT(BridgeActGap(Reason::Minigame, Reason::Minigame, 0) == Reason::Minigame);
    EXPECT(BridgeActGap(Reason::PauseMenu, Reason::Minigame, 1) == Reason::PauseMenu);
    EXPECT(BridgeActGap(Reason::None, Reason::Cutscene, 1) == Reason::None);
    EXPECT(BridgeActGap(Reason::None, Reason::None, 1) == Reason::None);
    // The tick after a bridged tick: the last reason is the held one, so the hold runs on to the cap.
    Reason held = Reason::Minigame;
    for (unsigned off = 1; off <= kMinigameGapTicks + 1; ++off)
    {
        held = BridgeActGap(Reason::None, held, off);
        EXPECT((held == Reason::Minigame) == (off <= kMinigameGapTicks));
    }
    // The counter step: 0 on a minigame tick, one more per other tick up to the cap, where it stays.
    EXPECT(NextTicksOff(Reason::Minigame, 7) == 0);
    EXPECT(NextTicksOff(Reason::None, 0) == 1);
    EXPECT(NextTicksOff(Reason::None, kMinigameGapTicks) == kMinigameGapTicks + 1);
    EXPECT(NextTicksOff(Reason::None, kMinigameGapTicks + 1) == kMinigameGapTicks + 1);
    EXPECT(NextTicksOff(Reason::PauseMenu, 1) == 2);
    // The two together, as Tick runs them: the one-tick gap between chained acts never shows; a longer gap shows
    // after exactly kMinigameGapTicks ticks and stays shown; another reason in the gap is its own and ends the hold.
    struct Run
    {
        Reason   last  = Reason::None;
        unsigned off   = kMinigameGapTicks + 1;  // the start: nothing was on
        unsigned flips = 0;
        Reason Tick(Reason raw)
        {
            off              = NextTicksOff(raw, off);
            const Reason now = BridgeActGap(raw, last, off);
            flips += (now != Reason::None) != (last != Reason::None);
            last = now;
            return now;
        }
    };
    Run chain;
    EXPECT(chain.Tick(Reason::Minigame) == Reason::Minigame);
    EXPECT(chain.Tick(Reason::None) == Reason::Minigame);
    EXPECT(chain.Tick(Reason::Minigame) == Reason::Minigame);
    EXPECT(chain.flips == 1);
    Run over;
    over.Tick(Reason::Minigame);
    for (unsigned i = 1; i <= kMinigameGapTicks + 3; ++i)
    {
        EXPECT((over.Tick(Reason::None) == Reason::Minigame) == (i <= kMinigameGapTicks));
    }
    EXPECT(over.flips == 2 && over.off == kMinigameGapTicks + 1);
    Run menu;
    menu.Tick(Reason::Minigame);
    EXPECT(menu.Tick(Reason::None) == Reason::Minigame);
    EXPECT(menu.Tick(Reason::PauseMenu) == Reason::PauseMenu);
    EXPECT(menu.Tick(Reason::None) == Reason::None);
    EXPECT(menu.flips == 2);
}

/// The signals compare field by field (the log's change test); the title by the field root's name.
void Compare()
{
    EXPECT(Readable() == Readable());
    auto changed     = Readable();
    changed.menuKind = 4;
    EXPECT(!(changed == Readable()));
    changed             = Readable();
    changed.minigameAct = 1;  // the owner is part of the change test
    EXPECT(!(changed == Readable()));
    EXPECT(IsTitle(L"005-TITLB") && !IsTitle(L"080-SLU5B") && !IsTitle(L""));
    for (int i = 0; i <= static_cast<int>(Reason::Title); ++i)
    {
        EXPECT(NameOf(static_cast<Reason>(i))[0] != L'?');
    }
}

}  // namespace

int main()
{
    NothingKnown();
    EachSignal();
    Order();
    ActGap();
    Compare();
    return lss::test::Report();
}
