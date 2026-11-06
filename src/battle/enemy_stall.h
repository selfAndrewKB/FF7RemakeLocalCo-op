#pragma once

// An enemy aimed at a human it cannot reach picks again: a MonoDrive at full ATB, aimed at a player 28 m away, was seen
// to wait 95 s with another player 0.7 m from it (research/RE-enemy-stall-repick.md, pass S-RMK-STALL).
//
// In single player every human target is the one character the party moves with. In co-op the hate rows give every
// human the player's weight (battle/enemy_coin's rule B), so a hate enemy's target moves to another human after each
// attack, at any distance. Many enemies' heavy attack tests its reach (the MonoDrive's Fire: within 20 m and in sight)
// and is simply not requested otherwise; at full ATB only the heavy event fires, and the game picks a hate enemy's
// target again only after an action, so the enemy waits until that human comes near.
// The co-op result is the single-player one: an enemy fights a human it can reach. The enemy's own decline is the reach
// test, and the game's own re-pick is the lever:
//   - the enemy component's attack-ready test (each tick; 1 light, 2 heavy) is redirected here and its answer passed
//     on unchanged; each enemy's ready span is timed (the game keeps no record of a decline);
//   - from the coin's gate redirect, before the game's gate runs: an enemy ready for two of its own light intervals
//     (LightAttachATBIntervalTime; at least 3 s) without acting is stalled. If the game would pick for it (a hate
//     enemy, no script target lock, no forced list, requests not blocked, no LookChange give-up of its own) and its
//     target is a standing human while another human stands, the mod sets the controller's pending hate byte, the one
//     write of Blueprint SetHateUpdateFlag. The gate then runs the game's hate update in the same call: the rows (the
//     declined human -200, every other human +200), the pick, the target. At most one decision per enemy per
//     threshold; an action ends the wait (battle/stall_rules.h, host-tested).
// Left to the game: single player and the coin off (`Tossing`), bosses, summons and mines (not hate enemies: the
// update returns at once for them), the edges above. Not built: the decline is not told from a refused request (a
// cooldown) by a hook on the request's accept; the threshold sits above every natural wait seen (5.3 s).
// Logged: the install (Info); each re-pick and its outcome, the next action or a stall again (Debug); each skip once
// per stall (Debug); a 60 s summary (Debug). `enemy-stall on|off` (on); reading `enemy-stall.counts`.
// Threads: the ready test and the gate run on the battle AI's thread; the byte is written on that thread right before
// the game's gate reads it there. The summary and the readings run on the game thread.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::enemy_stall {

/// Redirects the attack-ready test (shared with battle/enemy_watch's ready lines) and registers the switch and the
/// reading. False (logged) if a function differs from the research: no enemy is then re-picked, nor ready logged.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// The coin's gate redirect, for an enemy, before the game's gate: its controller, battle component and running action
/// (0: none). Sets the pending hate byte for a stalled enemy the game may pick for. Battle AI thread.
void BeforeGate(void* controller, void* component, std::uint64_t action) noexcept;

/// Game thread, once per frame: the summary.
void Tick() noexcept;

}  // namespace lss::battle::enemy_stall
