#pragma once

// The repair of level states a field-action spot's pass left on (research/RE-stuck-spot-repair.md): before the start
// signals of a further player's pass matched player 1's (world/traversal_action), such a pass sent player 1's start
// signals and not his end, and the state they started stayed on. The level keeps it in every save made in that field: a
// rail camera on player 1's camera manager that outranks every talk and look-at camera, a box left armed. Player 1's
// own pass through that spot, or the spot's end signals sent from the console (world/state_signal), end it.
//
// Off by default: since a seat's pass sends only its own member's signals, single-player passes and co-op passes alike
// send a start with its end, so only a save made before that carries such a state; `stuck.auto on` (or one
// `stuck.repair`) turns the repair on for the session when such a save is loaded.
// Switched on, this module does it by itself, as player 1's pass would: once a field is steady (a load, a field change, a retry) and
// whenever more spots stream in, with nobody in a field action, a talk, an event, a battle or a menu, it reads every
// loaded spot's passes against the active level states (world/stuck_spot_rules), dry-runs each held pass's end triggers
// over every state that waits on them (the census must be whole), and accepts the ones that move nothing but what that
// pass holds, through rows that save nothing, set nothing directly and clear no history, with no story act finished or
// started on the way. An accepted pass is sent only when a second scan 10 s later, nobody busy at either, finds it the
// same (a pass under way has ended by then), through the game's own sender, one pass at a time; half a second later the
// moved states are read back and the outcome logged. An end is sent at most once per field. Each verdict is logged once
// per field at Info (held, sent or refused and why), each scan at Debug; refused passes stay for player 1's pass or the
// console.
// The field's spots seen this session are remembered by the field's name: their level states belong to the field and stay
// in its slots after a spot streams out (an area change inside the field), so their passes are still judged there.
// After a repair reads back, once the repaired slots' act queues are idle (or 5 s on), the game's own save-time gather
// refreshes the field block: field init re-reads that block at the next area change, which would otherwise bring the old
// state back (research/RE-field-block-regather.md). Its one visible effect is a save's: the block's party places become
// the party's places then. A running rail camera act whose object rests in its hold state and that no known pass explains
// is logged once per field, not sent.
//
//   stuck.scan     the verdicts now, without sending
//   stuck.repair   scans now and sends what a second scan confirms (with stuck.auto off too)
//   stuck.auto     the repair at a field's load and as spots stream in (off)
//
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::world::stuck_spots {

/// Finds the field block's gather and the rail act's class, and registers the commands; false (logged) when the state
/// signal sender or the act tables are missing (a gather that is not as the research read it only warns: repairs then hold
/// until the next area change). Needs world/state_signal and world/traversal_walkup (its spot map); without
/// world/traversal_action nothing is ever sent (nobody's field action can be ruled out). The spot class is looked up when
/// first needed.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick.
void Tick() noexcept;

}  // namespace lss::world::stuck_spots
