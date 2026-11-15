#pragma once

// A level state trigger sent by name from the console, through the game's own sender (the call a walk-up exit makes
// for a spot's end triggers): the repair of a spot's level state left on by an unmatched start, as player 1's pass
// through the spot would end it (research/RE-cave-spot-camera-toggle.md, RE-send-signal.md).
//
//   state.trigger <name>   what the trigger's row would do (its scope, the objects it sets directly, its save step)
//   state.signal <name>    sends it, queued for the state manager's next tick, on the steady field only
//
// A send is the game's own: it moves only the states that wait for the trigger, but its row's direct sets apply
// whatever the objects' states are, its save step can write a save record, and the save keeps its history bit. So a
// trigger whose row sets objects directly or saves is refused; `state.trigger` says why.

namespace lss::platform {
class GameImage;
}

namespace lss::world::state_signal {

/// Finds the game's sender and registers the two commands; false (logged) when the sender is not as the research
/// read it. Needs world/data_rows.
bool Install(const platform::GameImage& image) noexcept;

}  // namespace lss::world::state_signal
