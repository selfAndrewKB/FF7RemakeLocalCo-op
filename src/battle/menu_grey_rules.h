#pragma once

// The open battle menu's grey under the slow-down switch (battle/menu_grey.h applies it), as pure rules over what the
// game's UI filter 0 shows: at the wait's spawn, at a menu-close restore, at a flip of the switch and at the tick that
// settles a debt. A debt is a skip or a stop that may have left another owner's count (a minigame's wait, a
// Blueprint's show) with no effect up; it is settled the game's unsuppress way once no battle menu waits (research/
// RE-menu-grey.md). Host-tested (tests/menu_grey_rules_test.cpp).

#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::battle::menu_grey_rules {

/// Filter 0 as the game keeps it: an effect up, the shared count, the suppressed byte.
struct Filter
{
    bool         effectUp   = false;
    std::int32_t count      = 0;
    bool         suppressed = false;
};

enum class Act : std::uint8_t
{
    Pass,     // the game's own call runs
    Skip,     // the game's call is skipped: a debt
    Nothing,  // nothing to do
    Stop,     // the effect is stopped with the game's deactivate: a debt
    Start,    // the effect is started with the game's activate (counted, unsuppressed, none up): the debt is paid
};

/// The wait's spawn of the grey (the count's 0 -> 1).
constexpr Act AtOpen(bool slows) noexcept
{
    return slows ? Act::Pass : Act::Skip;
}

/// A menu closing over the filters brings each counted one back; the wait's not while a battle menu waits with the
/// switch off.
constexpr Act AtRestore(bool slows, bool waitFilter, bool battleMenuWaits) noexcept
{
    return waitFilter && !slows && battleMenuWaits ? Act::Skip : Act::Pass;
}

/// The switch flipped: off stops a waiting menu's grey; on starts a counted, unsuppressed filter with no effect up, as
/// the game's own unsuppress does.
constexpr Act AtSwitch(bool slows, const Filter& filter, bool battleMenuWaits) noexcept
{
    if (!slows && filter.effectUp && battleMenuWaits)
    {
        return Act::Stop;
    }
    if (slows && !filter.effectUp && filter.count > 0 && !filter.suppressed)
    {
        return Act::Start;
    }
    return Act::Nothing;
}

/// A debt settled at the tick (the caller has checked that a debt is owed and that no battle menu waits): another
/// owner still counts the filter and nothing shows it, so it is started; else nothing is owed (the switch is on again
/// and settled it, or no other owner counts).
constexpr Act AtSettle(bool slows, const Filter& filter) noexcept
{
    return !slows && !filter.effectUp && filter.count > 0 && !filter.suppressed ? Act::Start : Act::Nothing;
}

/// Whether an act leaves a debt (Skip, Stop), pays one (Start), or neither.
constexpr bool Owes(Act act) noexcept
{
    return act == Act::Skip || act == Act::Stop;
}
constexpr bool Pays(Act act) noexcept
{
    return act == Act::Start;
}

// The wait's sound, the game's one muffle for the whole mix (research/RE-menu-sound-muffle.md): raised at a wait's open,
// lifted at its close, with no count. `waits` holds every player's wait state by place (0 player 1, then the seats), 0 for
// a player with no menu: 0 none, 1 opening, 5 open, 4 closing.

/// A wait state that has raised the sound and has not begun to close.
constexpr bool Waiting(std::int32_t wait) noexcept
{
    return wait == 1 || wait == 5;
}

/// The close's sound is held back while another menu still waits, so the first of two menus to close does not lift the
/// other's. The menus tick in order (player 1's first, then each seat's): one ticking later still counts while closing, so
/// of several closing in one frame the last plays it. `me`: the ticking menu's place. Rebirth's rule (battle/menu_rules.h).
constexpr bool HoldSoundOff(int me, std::span<const std::int32_t> waits) noexcept
{
    for (int other = 0; other < static_cast<int>(waits.size()); ++other)
    {
        const std::int32_t wait = waits[static_cast<std::size_t>(other)];
        if (other != me && (Waiting(wait) || (other > me && wait == 4)))
        {
            return true;
        }
    }
    return false;
}

/// What the switch's flip owes the sound while menus wait. Switched off: lifted if any wait raised it, a closing one too,
/// since its own close will now be skipped. Switched on: raised if any menu still waits, and its close then lifts it.
enum class SoundFlip : std::uint8_t
{
    Nothing,
    Raise,
    Lift,
};
constexpr SoundFlip AtSoundSwitch(bool slows, std::span<const std::int32_t> waits) noexcept
{
    for (const std::int32_t wait : waits)
    {
        if (slows ? Waiting(wait) : wait != 0)
        {
            return slows ? SoundFlip::Raise : SoundFlip::Lift;
        }
    }
    return SoundFlip::Nothing;
}

}  // namespace lss::battle::menu_grey_rules
