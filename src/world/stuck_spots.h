#pragma once

// The repair of level states a field-action spot's pass left on (research/RE-stuck-spot-repair.md): before the start
// signals of a further player's pass matched player 1's (world/traversal_action), such a pass sent player 1's start
// signals and not his end, and the state they started stayed on. The level keeps it in every save made in that field: a
// rail camera on player 1's camera manager that outranks every talk and look-at camera, a box left armed. Player 1's
// own pass through that spot, or the spot's end signals sent from the console (world/state_signal), end it.
//
// This module does it by itself, as player 1's pass would: once a field is steady (a load, a field change, a retry) and
// whenever more spots stream in, with nobody in a field action, a talk, an event, a battle or a menu, it reads every
// loaded spot's passes against the active level states (world/stuck_spot_rules), dry-runs each held pass's end triggers
// over every state that waits on them (the census must be whole), and accepts the ones that move nothing but what that
// pass holds, through rows that save nothing, set nothing directly and clear no history, with no story act finished or
// started on the way. An accepted pass is sent only when a second scan 10 s later, nobody busy at either, finds it the
// same (a pass under way has ended by then), through the game's own sender, one pass at a time; half a second later the
// moved states are read back and the outcome logged. An end is sent at most once per field. Each verdict is logged once
// per field at Info (held, sent or refused and why), each scan at Debug; refused passes stay for player 1's pass or the
// console.
//
//   stuck.scan     the verdicts now, without sending
//   stuck.repair   scans now and sends what a second scan confirms (with stuck.auto off too)
//   stuck.auto     the repair at a field's load and as spots stream in (on)
//
// Game thread only.

namespace lss::world::stuck_spots {

/// Registers the commands; false (logged) when the state signal sender or the act tables are missing. Needs
/// world/state_signal and world/traversal_walkup (its spot map); without world/traversal_action nothing is ever sent
/// (nobody's field action can be ruled out). The spot class is looked up when first needed.
bool Install() noexcept;

/// Game thread, each engine tick.
void Tick() noexcept;

}  // namespace lss::world::stuck_spots
