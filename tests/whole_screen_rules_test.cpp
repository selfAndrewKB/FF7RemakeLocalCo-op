// Host-side tests for views/whole_screen_rules.h: which game signals take the whole screen (the one-window single
// view's reasons), in their order. Rebirth's tests/whole_screen_test.cpp gave the shape; the signals are Remake's.

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
    s          = Readable();
    s.cutscene = true;
    EXPECT(ReasonOf(s) == Reason::Cutscene);
    s            = Readable();
    s.sceneHolds = true;
    EXPECT(ReasonOf(s) == Reason::Scene);
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
/// a menu over a scene, a cutscene over the lock that comes with it, a skip over a held black.
void Order()
{
    auto s       = Readable();
    s.gameState  = kStateCinemaPause;
    s.cutscene   = true;
    s.sceneHolds = true;
    EXPECT(ReasonOf(s) == Reason::CinemaPause);
    s            = Readable();
    s.gameState  = kStatePauseMenu;
    s.shopUp     = true;  // a shop re-opened under a pause menu
    s.scriptMenu = true;
    EXPECT(ReasonOf(s) == Reason::PauseMenu);
    s            = Readable();
    s.shopUp     = true;
    s.sceneHolds = true;  // a shop holds the field lock too
    EXPECT(ReasonOf(s) == Reason::Shop);
    s            = Readable();
    s.cutscene   = true;
    s.sceneHolds = true;
    s.skippable  = true;
    EXPECT(ReasonOf(s) == Reason::Cutscene);
    s          = Readable();
    s.bike     = true;
    s.cutscene = true;  // a chase's opening scene: the chase holds through it
    EXPECT(ReasonOf(s) == Reason::Bike);
    // A VR or colosseum session: its fight stays split (78 with the bit in a battle, the lock not counted: the read
    // leaves sceneHolds false in a battle), its menu collapses, a pause over it is the pause's.
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

/// The signals compare field by field (the log's change test); the title by the field root's name.
void Compare()
{
    EXPECT(Readable() == Readable());
    auto changed     = Readable();
    changed.menuKind = 4;
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
    Compare();
    return lss::test::Report();
}
