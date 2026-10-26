#pragma once

// One of the game's action getters (Input_IsActionHeld_ByKey, Input_IsActionPressed_ByKey) on a copy of an action key,
// as the game calls them (research/RE-input.md): the key is 24 bytes (FName, details object, reference controller),
// passed by value, and the getter releases the copy's reference on the controller, so the copy takes one first (the
// game's own ++ on the shared count). Read inside a player's pad_buttons::ButtonScope, the answer is that player's
// pad's (players/pad_buttons.h). Game thread.

#include <cstdint>

namespace lss::players::action_key {

using Getter = bool (*)(void* key);

/// `getter` on a copy of the 24-byte key at `key` (live address). False when the key, the reference or the call could
/// not be made (`out` is then unchanged).
bool Test(Getter getter, std::uintptr_t key, bool& out) noexcept;

}  // namespace lss::players::action_key
