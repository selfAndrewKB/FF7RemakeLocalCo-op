#pragma once

// A level state trigger sent by name, through the game's own sender (the call a walk-up exit makes for a spot's end
// triggers): the repair of a spot's level state left on by an unmatched start, as player 1's pass through the spot
// would end it (research/RE-cave-spot-camera-toggle.md, RE-send-signal.md). From the console:
//
//   state.trigger <name>   what the trigger's row would do (its scope, the objects it sets directly, its save step)
//   state.signal <name>    sends it, queued for the state manager's next tick, on the steady field only
//
// and from world/stuck_spots, which finds such states at a load and sends the spot's end triggers itself.
// A send is the game's own: it moves only the states that wait for the trigger, but its row's direct sets apply
// whatever the objects' states are, its save step can write a save record, its history list clears other triggers'
// fired bits (story conditions read them), and the save keeps its own fired bit. So a trigger whose row sets objects
// directly, saves, runs a backup act or clears history is refused; `state.trigger` says why.

#include <cstdint>
#include <string>
#include <vector>

namespace lss::platform {
class GameImage;
}

namespace lss::world::state_signal {

/// A trigger's row as a send would apply it. `readable` false when any of the fields a refusal rests on could not be
/// read: such a row is refused.
struct RowReading
{
    std::uint64_t             name         = 0;
    const std::uint8_t*       row          = nullptr;
    bool                      readable     = false;
    std::uint8_t              scope        = 0;  // 0 the sender's own, 1 and 2 every active state that waits for it, 3 listed objects
    std::uint8_t              saveStep     = 0;
    std::int32_t              directSets   = 0;
    std::int32_t              clearHistory = 0;
    std::uint64_t             backupAct    = 0;
    std::vector<std::int64_t> objects, states, sendObjects;
};

/// Finds the game's sender and registers the two commands; false (logged) when the sender is not as the research
/// read it. Needs world/data_rows.
bool Install(const platform::GameImage& image) noexcept;

/// The sender was found at install.
bool Installed() noexcept;

/// The row of the trigger named `name` (its 8 bytes). May throw (allocation): callers catch.
RowReading ReadRow(std::uint64_t name);

/// Why a send of this row is refused, empty when it may go. May throw (allocation): callers catch.
std::wstring Refusal(const RowReading& r);

/// Sends the trigger through the game's sender with the walk-up exit's arguments, queued for the state manager's next
/// tick; the caller has checked the row and that the field is steady. False when the call faulted (logged). Game thread.
bool Send(std::uint64_t name) noexcept;

}  // namespace lss::world::state_signal
