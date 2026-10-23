#pragma once

// Each further player's member at the player's rates (RE-battle-member-two.md §2, L1; Rebirth's player_two/player_hits).
//
// Every "is this the player's character" test in battle is the leader test, `comp+0x58 == PM+0x233`, so player
// 2's member counts as an AI ally wherever the test runs outside player 2's scopes: its passive ATB fills at
// the AI-ally coefficient (half), its guard ATB too, its abilities carry no "player's ability" bit (the on-hit ATB
// coefficient and the enemy's reaction read it), and its normal-attack staggers earn no reward. The game's own
// functions run unchanged, inside the seat's leader scope answering that seat's member, for exactly the calls made on
// a seat's member's component (its battle component is `this`) or with its character as the attacker:
//   - the component's passive ATB tick (vtable slot +0x318 of the eight character components); its rate getter
//     and the guard coefficient are both read inside it;
//   - the component's "ability by the player" flag setter (slot +0x748);
//   - the enemy's stagger-gauge hit (one call site), where the attacker is an argument.
// And the turn during a roll (RE-roll-direction-two.md): the component's tick turns its character toward the game's
// merged move stick rotated by PartyManager+0's camera while an animation's turn window is open, so a further
// player's roll bent toward player 1's stick in player 1's camera. That call (one site in the component's tick) runs
// in the seat's primary scope (its camera basis) with its own pad's stick.
// The leader scope writes the party manager's leader bytes, which only the game thread may do: a call made off it
// runs the game's function unchanged and is counted (logged once). Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::member_rates {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick: notes the game thread and each seat's member's component.
void Tick() noexcept;

}  // namespace lss::battle::member_rates
