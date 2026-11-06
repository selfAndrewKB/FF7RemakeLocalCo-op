#pragma once

// When an enemy that is ready to attack but declines counts as stalled, and whether the game may pick again for it
// (battle/enemy_stall; research/RE-enemy-stall-repick.md). Pure: no engine access, host-tested.

#include <cstdint>

namespace lss::battle::stall_rules {

inline constexpr std::int64_t kFloorMs     = 3000;   // a short light interval (the Hellhound's 1.0 s) still waits this long
inline constexpr std::int64_t kCeilingMs   = 60000;  // an unread or odd interval never makes the wait longer than this
inline constexpr double       kIntervals   = 2.0;    // above every natural wait seen (5.3 s), well below the stalls

/// The wait after which a ready enemy that has not acted counts as stalled: two of its own light intervals
/// (LightAttachATBIntervalTime, seconds), between the floor and the ceiling. An unread interval (not a positive finite
/// number) gives the floor.
constexpr std::int64_t ThresholdMs(float intervalSeconds) noexcept
{
    if (!(intervalSeconds > 0.0f) || !(intervalSeconds < 1.0e6f))
    {
        return kFloorMs;
    }
    const auto ms = static_cast<std::int64_t>(static_cast<double>(intervalSeconds) * kIntervals * 1000.0 + 0.5);  // rounded: 3.3f is 3.2999...
    return ms < kFloorMs ? kFloorMs : ms > kCeilingMs ? kCeilingMs : ms;
}

/// Whether the wait is over: ready since `readySinceMs` (0: not ready) for the threshold, and the last decision on
/// this enemy (a re-pick or a skip; 0: none) at least one threshold ago.
constexpr bool Due(std::int64_t nowMs, std::int64_t readySinceMs, std::int64_t lastDecisionMs, std::int64_t thresholdMs) noexcept
{
    return readySinceMs != 0 && nowMs - readySinceMs >= thresholdMs && (lastDecisionMs == 0 || nowMs - lastDecisionMs >= thresholdMs);
}

/// Why a stalled enemy is left to the game, or None: the game picks again.
enum class Skip : std::uint8_t
{
    None,
    Unread,         // a field the rule reads could not be read
    NotHate,        // bosses, summons, mines: the game's hate update returns at once for them
    Locked,         // the script holds its target (LockTargetCharaBase): the game refuses the pick
    ForcedList,     // the update's list is filled: it would only pick again from it
    Blocked,        // the script blocks its own requests on purpose
    GivesUpItself,  // the game's own give-up (LookChange) runs for this enemy
    NotAHuman,      // its target is no human's member (an AI ally, or none)
    TargetDown,     // its target is knocked out: the game picks again by itself
    Alone,          // no other human stands: the single-player wait for that human
};

/// What the rule reads about a stalled enemy and its target.
struct Facts
{
    bool read           = false;  // every field below was read
    bool hate           = false;  // BattleComp_IsHateEnemy
    bool locked         = false;  // its component's target lock is set
    bool forcedList     = false;  // the update's list count is above 0
    bool blocked        = false;  // its request flags block requests
    bool givesUpItself  = false;  // its LookChange mode is not 0
    int  targetSeat     = -1;     // the human seat whose member it targets; -1 none
    bool targetStanding = false;  // that member is not knocked out
    bool otherStanding  = false;  // another human's member stands
};

/// The first reason in the table's order that leaves the enemy to the game, or None.
constexpr Skip Judge(const Facts& f) noexcept
{
    if (!f.read)
    {
        return Skip::Unread;
    }
    if (!f.hate)
    {
        return Skip::NotHate;
    }
    if (f.locked)
    {
        return Skip::Locked;
    }
    if (f.forcedList)
    {
        return Skip::ForcedList;
    }
    if (f.blocked)
    {
        return Skip::Blocked;
    }
    if (f.givesUpItself)
    {
        return Skip::GivesUpItself;
    }
    if (f.targetSeat < 0)
    {
        return Skip::NotAHuman;
    }
    if (!f.targetStanding)
    {
        return Skip::TargetDown;
    }
    if (!f.otherStanding)
    {
        return Skip::Alone;
    }
    return Skip::None;
}

inline constexpr int kSkipCount = static_cast<int>(Skip::Alone) + 1;

}  // namespace lss::battle::stall_rules
