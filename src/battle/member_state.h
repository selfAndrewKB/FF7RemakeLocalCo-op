#pragma once

// A party member's battle state as the game keeps it on its battle player component, read guarded (any thread). One
// reader for the knocked-out test, which the enemies' leader follow, the enemy coin and the battle switch all ask, and
// for the locked-action test the arena warp and a trade between players in a fight ask.

#include "engine_facts.h"
#include "platform/guarded.h"
#include "players/actor_fields.h"

#include <cstdint>
#include <optional>

namespace lss::battle::member_state {

/// The action state byte of `character`'s battle component; empty when the character or its component cannot be read.
inline std::optional<std::uint8_t> ActionState(const void* character) noexcept
{
    const auto*  component = players::actor_fields::BattleComponent(character);
    std::uint8_t state     = 0;
    if (component == nullptr || !platform::Read(component + facts::kBattleCompActionState, state))
    {
        return std::nullopt;
    }
    return state;
}

/// `character` lies knocked out (its component in the KO state). `unreadableCounts`: what a character whose state
/// cannot be read answers (nobody to follow for the enemies, not standing for the coin; not down for a switch).
inline bool KnockedOut(const void* character, bool unreadableCounts) noexcept
{
    const auto state = ActionState(character);
    return state ? *state == facts::kBattleCompStateKnockedOut : unreadableCounts;
}

/// `character` is in an action the game's own leader switch does not leave (its component in the locked state without
/// the may-leave flags: research/RE-battle-switch.md). A character whose state cannot be read counts as in one: nothing
/// is moved under a state unread.
inline bool InLockedAction(const void* character) noexcept
{
    const auto*  component = players::actor_fields::BattleComponent(character);
    std::uint8_t flags     = 0;
    const auto   state     = ActionState(character);
    if (!state || component == nullptr || !platform::Read(component + facts::kBattleCompFlags, flags))
    {
        return true;
    }
    return *state == facts::kBattleActionLocked && (flags & facts::kBattleCompMayLeave) == 0;
}

}  // namespace lss::battle::member_state
