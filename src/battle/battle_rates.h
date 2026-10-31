#pragma once

// The battle's rates: enemy damage, enemy HP, boss HP, the enemies' stagger gauge, EXP and AP, each a multiplier from
// 0.1 to 10.0 set live from the Co-op page, the console or the settings file (platform/settings.h; the user,
// 2026-10-31; plan/topics/battle.md History 16:11). Each one drives the game's own computation where the game has one
// (research/RE-enemy-stat-scaling.md, RE-streamlined-progression.md):
//   - HP and the stagger gauge: the game computes an enemy's from its tables in one function, at the enemy's battle
//     start and at each phase change (the spec override). Its two enemy calls are redirected: the game's values are
//     scaled before the game sets them, the boss rate for an enemy whose own row marks it the fight's leader
//     (CharacterKindID 1, the game's own boss notion: its death rewards every enemy left), the enemy rate otherwise;
//     a party-side unit (a summon) is left as the game made it. A new rate reaches the enemies already fighting
//     through the game's own phase change, re-run with each enemy's current row: max HP and the gauge change, their
//     shares of HP and gauge left stay (the game's own path, so no bar jumps and no percent trigger fires).
//   - Enemy damage to the party: on Easy and Hard the game scales each enemy hit on the party by a rate row of its
//     own, which takes the multiplier (the row the game reads, written times it and put back at removal). On Normal
//     the game has no such step and no row is read, so the step is the mod's: the same conditions (an enemy-side
//     attacker, a party-side victim, not a heal, a damage kind the game's step sees) and the same rounding, at the
//     same place (the entry of the clamp that follows the game's step), before the game's 9999 cap. Poison ticks and
//     preset results are not hits through that step, in the game's either.
//   - EXP and AP: the game's own bonus rates (200 % and 300 %, the post-story bonus and the 2x EXP & 3x AP option),
//     read at each award. For the one award call the rows carry the game's rate times the multiplier (100 % times it
//     while the game's bonus is off, its gate then answered open for that call alone), then are put back. The
//     rounding, each member's share and the result screen stay the game's. The save's own option byte is never
//     written.
// Logging: Info at each change of a rate and at install; Debug at each scaled enemy, award and hit (only while its
// rate is not 1.0). Hits and stats may run on any thread (the rates are atomics); the rows and the re-run are the game
// thread's (Tick).

namespace lss::platform {
class GameImage;
}

namespace lss::battle::battle_rates {

/// Finds the game's functions and places the redirects (each lever on its own: one that cannot be placed is logged
/// and its rate stays the game's), then reads the settings and registers the six options. At on_unreal_init, before
/// any battle (one call crosses a code block).
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread, once per frame: the damage rate rows kept, a new HP or gauge rate taken by the enemies fighting.
void Tick() noexcept;

}  // namespace lss::battle::battle_rates
