// Host-side tests for the grey's rules under the slow-down switch (battle/menu_grey_rules.h). Run without the game.

#include "battle/menu_grey_rules.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace lss::test {
inline int g_failures = 0;
inline int Report()
{
    std::printf(g_failures == 0 ? "menu_grey_rules: all passed\n" : "menu_grey_rules: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
}  // namespace lss::test

#define EXPECT(condition)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);     \
            ++lss::test::g_failures;                                         \
        }                                                                    \
    } while (false)

namespace {

using namespace lss::battle::menu_grey_rules;

constexpr Filter kNone{false, 0, false};
constexpr Filter kCounted{false, 1, false};        // another owner counts it, nothing up
constexpr Filter kUp{true, 1, false};              // the grey shows
constexpr Filter kSuppressed{false, 1, true};      // counted but suppressed by the game

void Open()
{
    static_assert(AtOpen(true) == Act::Pass);
    static_assert(AtOpen(false) == Act::Skip);
    static_assert(Owes(AtOpen(false)) && !Owes(AtOpen(true)));
}

void Restore()
{
    static_assert(AtRestore(false, true, true) == Act::Skip);   // off, the wait's filter, a battle menu waits
    static_assert(AtRestore(true, true, true) == Act::Pass);    // on: every call is the game's
    static_assert(AtRestore(false, false, true) == Act::Pass);  // another filter type
    static_assert(AtRestore(false, true, false) == Act::Pass);  // no battle menu waits: the game's restore
}

void Switch()
{
    static_assert(AtSwitch(false, kUp, true) == Act::Stop);        // off while a menu waits with the grey up
    static_assert(AtSwitch(false, kUp, false) == Act::Nothing);    // off with no battle menu waiting: not ours to stop
    static_assert(AtSwitch(false, kNone, true) == Act::Nothing);   // off, nothing up
    static_assert(AtSwitch(true, kCounted, false) == Act::Start);  // on: a counted, unsuppressed filter with none up
    static_assert(AtSwitch(true, kUp, true) == Act::Nothing);      // on, already up
    static_assert(AtSwitch(true, kSuppressed, true) == Act::Nothing);
    static_assert(AtSwitch(true, kNone, true) == Act::Nothing);
    static_assert(Owes(Act::Stop) && Pays(Act::Start) && !Owes(Act::Nothing) && !Pays(Act::Nothing));
}

void Settle()
{
    static_assert(AtSettle(false, kCounted) == Act::Start);     // another owner still counts it, nothing shows it
    static_assert(AtSettle(true, kCounted) == Act::Nothing);    // the switch is on again: it settled the debt itself
    static_assert(AtSettle(false, kUp) == Act::Nothing);        // something shows it
    static_assert(AtSettle(false, kNone) == Act::Nothing);      // no other owner
    static_assert(AtSettle(false, kSuppressed) == Act::Nothing);
}

}  // namespace

/// The muffle: a close is held back while another menu waits; of several closing in one frame the last plays it; a flip
/// follows the waits, a closing one included when switched off.
void Sound()
{
    using Waits = std::array<std::int32_t, 3>;
    constexpr Waits none{0, 0, 0}, oneOpen{5, 4, 0}, oneOpening{1, 4, 0}, bothClosing{4, 4, 0}, alone{4, 0, 0};
    static_assert(HoldSoundOff(1, oneOpen));       // player 2 closes, player 1's still open: stays up
    static_assert(HoldSoundOff(1, oneOpening));    // player 1's only opening: it has raised it too
    static_assert(!HoldSoundOff(0, alone));        // the only menu closes
    static_assert(HoldSoundOff(0, bothClosing));   // player 1 ticks first: player 2's close, later this frame, plays it
    static_assert(!HoldSoundOff(1, bothClosing));  // the last to tick plays it
    static_assert(!HoldSoundOff(2, Waits{4, 4, 4}));
    static_assert(HoldSoundOff(2, Waits{0, 5, 4}));

    static_assert(AtSoundSwitch(false, none) == SoundFlip::Nothing);
    static_assert(AtSoundSwitch(true, none) == SoundFlip::Nothing);
    static_assert(AtSoundSwitch(false, Waits{0, 5, 0}) == SoundFlip::Lift);
    static_assert(AtSoundSwitch(false, alone) == SoundFlip::Lift);   // closing: its own lift is about to be skipped
    static_assert(AtSoundSwitch(true, alone) == SoundFlip::Nothing);  // closing: no raise, and its lift is harmless
    static_assert(AtSoundSwitch(true, Waits{0, 0, 1}) == SoundFlip::Raise);
}

int main()
{
    Open();
    Restore();
    Switch();
    Settle();
    Sound();
    return lss::test::Report();  // every check above is a static_assert
}
