#pragma once

// Each further player's own field target icons (research/RE-field-icons-two.md): the blue talk/examine/chest markers the
// target pick shows, made, updated and swept by the game for that player and drawn in that player's view only.
//
// The game keeps one icon widget per target (the pool item's +0x78), shown and updated by its pick and placed each frame
// through the widget's own player (its context at +0x1E8, set once at creation from "the first local player"). So:
//   - each seat has its own 256 widget pointers, exchanged with the items' around the seat's pick and press (as
//     world/field_targets exchanges its "was current" bytes); the game's own pick then creates, sets, gauges, moves and
//     sweeps the seat's widgets. Also exchanged around the pick: the items' location-dirty bytes (posted outside the
//     pick and consumed by the first pick that meets them: each seat keeps the ones posted since its last run) and the
//     icon manager's one "last gauge ratio" (else the gauge-full sound replays when the players' ratios differ);
//   - a widget made in a seat's pick gets the seat's local player as its owner: the widget creation's one "first local
//     player" call answers it, only inside that pick's Show call (any other caller is the game's); after Show the owner is
//     checked, and a widget that is not the seat's is destroyed (counted);
//   - the one function that frees a target destroys the seats' widgets of that target first (entry redirect); each
//     tick the seats' widgets are checked against the active targets and the object array (a widget whose target left is
//     destroyed, one the engine destroyed is forgotten); a seat's widgets are destroyed when its player leaves or changes
//     member, and with the passes when a run faults;
//   - per HUD pass the game's slot-15 tick runs again with the HUD step at 0 after the pass's host set (every icon placed
//     through its own player into that view: hud/second_image.h), and hud/pass_hide.h makes transparent the icons that are
//     not the pass's player's (OwnerSeat).
// Player 1's widgets are the game's own throughout. Game thread only, but the free redirect, which queues a target freed
// on another thread for the next tick.

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::target_icons {

/// Checks every site against the research. False (logged) if one differs: further players' picks then do not run.
bool Install(const platform::GameImage& image) noexcept;
/// Game thread, outside the pick: places the Show call, the widget creation's owner call (crosses a block) and the
/// free redirect, once. False if they are not all placed (then none is).
bool Place() noexcept;
/// All of them placed.
bool Placed() noexcept;
/// Destroys every seat's widgets (game thread) and restores what can be restored.
void Remove() noexcept;

/// Game thread, each engine tick before the pick: the seats' widgets checked (and the frees queued off the thread).
void Tick(void* mgr) noexcept;
/// Inside the pick redirect, before player 1's run: the location refreshes posted since are kept for every seat.
void BeforePick(void* mgr) noexcept;

/// The seat's widgets (and, for its pick, its location-dirty bytes and gauge ratio) exchanged in for one run of the game's
/// pick or press; `localPlayer` owns the widgets the run makes. Held() false: nothing exchanged (a fault put back what was).
class SeatSwap
{
public:
    SeatSwap(int seat, void* mgr, void* localPlayer, bool pick) noexcept;
    ~SeatSwap();
    SeatSwap(const SeatSwap&)            = delete;
    SeatSwap& operator=(const SeatSwap&) = delete;

    bool Held() const noexcept { return held_; }

private:
    int   seat_         = 0;
    void* mgr_          = nullptr;
    bool  pick_         = false;
    bool  held_         = false;
    bool  gaugeSwapped_ = false;
};

/// Destroys the seat's widgets with the game's destroy (game thread): its player left, changed member or local player.
void DropSeat(int seat) noexcept;

inline constexpr std::size_t kItems = 256;  // the target pool (facts::kTargetsItems)

/// The pool items on the manager's active list. False if the list could not be read whole.
bool ActiveItems(void* mgr, std::bitset<kItems>& active) noexcept;

/// The seat whose widget this is (1..), or 0 (player 1's or not an icon of a seat). Game thread.
int OwnerSeat(const void* widget) noexcept;
/// A seat holds at least one widget now. Game thread.
bool AnySeatIcons() noexcept;

/// Game thread, from the overlay driver after a pass's host set is written: every icon placed again through its own
/// player into this pass's view (the game's slot-15 tick, HUD step 0). Nothing while no seat holds an icon.
void PlaceForPass() noexcept;

/// For the `field.targets` reading.
std::wstring Report();

}  // namespace lss::world::target_icons
