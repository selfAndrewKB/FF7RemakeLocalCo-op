#pragma once

// A party member's battle state as the game keeps it on its battle player component, read guarded (any thread). One
// reader for the knocked-out test, which the enemies' leader follow, the enemy coin and the battle switch all ask.

#include "engine_facts.h"
#include "platform/guarded.h"

#include <cstdint>
#include <optional>

namespace lss::battle::member_state {

/// The action state byte of `character`'s battle component; empty when the character or its component cannot be read.
inline std::optional<std::uint8_t> ActionState(const void* character) noexcept
{
    const auto*  component = static_cast<const std::uint8_t*>(platform::ReadPointer(character, facts::kCharacterBattleComponent));
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

}  // namespace lss::battle::member_state
