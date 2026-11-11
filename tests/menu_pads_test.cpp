// Host-side tests for players/menu_pads.h: where a further pad's key events go while the game's menus need them.

#include "players/menu_pads.h"

#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

namespace mp = lss::players::menu_pads;
namespace at = lss::players::action_tables;

constexpr std::uint64_t kStart = 0x100, kPause = 0x200, kMap = 0x300, kIntel = 0x400, kAttack = 0x500, kCross = 0x600;

void TestKeys()
{
    mp::Keys keys;
    EXPECT(mp::Where(keys, kStart) == 0);
    EXPECT(mp::Mark(keys, kStart, mp::kToCopy));
    EXPECT(mp::Where(keys, kStart) == mp::kToCopy);
    EXPECT(mp::Mark(keys, kStart, mp::kToGame));  // the opening press replayed: both
    EXPECT(mp::Where(keys, kStart) == (mp::kToCopy | mp::kToGame));
    EXPECT(!mp::Mark(keys, 0, mp::kToCopy));  // no key name: not recorded
    EXPECT(mp::Where(keys, 0) == 0);
    mp::Forget(keys, kStart);
    EXPECT(mp::Where(keys, kStart) == 0);

    // Full: a new key is not recorded, a recorded one still is marked.
    for (std::uint64_t i = 1; i <= mp::Keys::kMax; ++i)
    {
        EXPECT(mp::Mark(keys, i, mp::kToCopy));
    }
    EXPECT(!mp::Mark(keys, kCross, mp::kToGame));
    EXPECT(mp::Where(keys, kCross) == 0);
    EXPECT(mp::Mark(keys, 3, mp::kToGame));
    EXPECT(mp::Where(keys, 3) == (mp::kToCopy | mp::kToGame));
    mp::Forget(keys, 5);
    EXPECT(mp::Mark(keys, kCross, mp::kToGame));  // the freed entry is reused
    EXPECT(mp::Where(keys, kCross) == mp::kToGame);
}

void TestSharedPlay()
{
    using lss::views::whole_screen::Reason;
    // Every reason there is: only a minigame and a bike chase share the pads (a menu has the game's own test).
    constexpr Reason kAll[] = {Reason::None,      Reason::PauseMenu,   Reason::CinemaPause,    Reason::ChapterPause,
                               Reason::ScriptMenu, Reason::Shop,       Reason::PauseWidget,    Reason::ArenaMenu,
                               Reason::Minigame,  Reason::Bike,        Reason::Cutscene,       Reason::EventCamera,
                               Reason::SkippableScene, Reason::CutsceneSkip, Reason::Black,    Reason::Loading,
                               Reason::Title};
    for (const Reason reason : kAll)
    {
        const bool shared = reason == Reason::Minigame || reason == Reason::Bike;
        EXPECT(mp::SharedPlay(reason) == shared);
    }
}

void TestRoute()
{
    mp::Keys keys;
    // New presses: the copy outside a shared moment, the game inside it.
    EXPECT(mp::Route(keys, kCross, false) == mp::kToCopy);
    EXPECT(mp::Route(keys, kCross, true) == mp::kToGame);
    // A key pressed into the copy before a menu opened repeats and releases there, even in menu time.
    mp::Mark(keys, kCross, mp::kToCopy);
    EXPECT(mp::Route(keys, kCross, true) == mp::kToCopy);
    // A key pressed into the game in menu time releases there after the menu closed.
    mp::Mark(keys, kAttack, mp::kToGame);
    EXPECT(mp::Route(keys, kAttack, false) == mp::kToGame);
    // The replayed opening press releases in both.
    mp::Mark(keys, kStart, mp::kToCopy | mp::kToGame);
    EXPECT(mp::Route(keys, kStart, false) == (mp::kToCopy | mp::kToGame));
    EXPECT(mp::Route(keys, kStart, true) == (mp::kToCopy | mp::kToGame));
}

void TestPressed()
{
    at::Shape shape;
    shape.names = {kPause, 0, kAttack, kStart, kMap};
    std::vector<at::State> before(5), after(5);
    after[0] = {0, 1, 0, 1};  // pressed now
    after[1] = {0, 1, 0, 1};  // a free index: never reported
    after[2] = {1, 1, 0, 0};  // held from before, no latch
    before[3] = {0, 1, 0, 1};  // already latched this frame by an earlier event
    after[3]  = {0, 1, 0, 1};
    after[4] = {1, 0, 0, 1};  // released now (latch set, not held)
    std::vector<std::uint64_t> out{kIntel};
    mp::Pressed(shape, before, after, out);
    EXPECT(out.size() == 1 && out[0] == kPause);

    std::vector<at::State> shorter(4);
    mp::Pressed(shape, shorter, after, out);
    EXPECT(out.empty());
}

void TestOpensMenu()
{
    const std::vector<std::uint64_t> openers{kPause, kMap, kIntel};
    const auto opens = [&](std::vector<std::uint64_t> pressed, std::uint64_t key) {
        return mp::OpensMenu(pressed, openers, key);
    };
    EXPECT(opens({kPause}, kStart));
    EXPECT(opens({kPause, kStart}, kStart));  // the raw key's own element beside the action
    EXPECT(opens({kMap, kIntel}, kStart));     // both touch-pad actions
    EXPECT(!opens({}, kStart));                // nothing pressed (a repeat)
    EXPECT(!opens({kStart}, kStart));          // the raw key alone opens nothing
    EXPECT(!opens({kPause, kAttack}, kStart)); // an opener bound with a gameplay action: the game must not see it
    EXPECT(!opens({kAttack}, kStart));
    const std::vector<std::uint64_t> unknown{0, 0, 0};
    EXPECT(!mp::OpensMenu(std::vector<std::uint64_t>{kPause}, unknown, kStart));  // openers unread: never
}

}  // namespace

int main()
{
    TestKeys();
    TestSharedPlay();
    TestRoute();
    TestPressed();
    TestOpensMenu();
    if (g_failures != 0)
    {
        std::printf("menu_pads: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("menu_pads: all passed\n");
    return 0;
}
