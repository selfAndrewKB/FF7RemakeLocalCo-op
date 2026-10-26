#pragma once

// A seat's nav-area component ticks in that seat's scopes (RE-operator-settings-two.md §2.5).
//
// Each End player controller owns a nav-area component whose tick diffs its own leader's nav areas and, on an
// InDoor or Town volume's enter and leave, pushes or pops that volume's camera setting on "the" field camera
// operator (the field root's weak), and marks the leader actor and the primary controller. A seat's controller
// gets the component with the game's own InitPartyLevel, so it ticked with no scope: Aerith's enters landed on
// player 1's operator, her leaves popped the key Cloud's enter had pushed there (player 1's camera lost InDoor
// while Cloud was still inside), and player 2's operator never got InDoor or Town.
// The lever: the component's tick slot runs the game's tick inside the seat's primary, operator and leader scopes
// when the component is that seat's controller's, so the same code reaches the seat's operator, member and
// controller. Game thread only (a component tick).
//
// A respawned operator (the member given back for a scene, then re-seated) misses the setting a volume pushed
// before it existed: the component's map of entered volumes lives on the controller, and the diff reports Stay for
// a volume it already lists, never Enter again. The game has no path that forgets the map (RE-nav-area-reenter.md:
// its camera reset runs the same diff out of band on player 1's component; only a new component starts empty).
// At each spawn of a seat's operator, inside its scopes: the game's diff run once (volumes left during the scene
// Leave, new ones Enter on the new operator), the listed keys forgotten with the map's own Remove, the diff run
// again so that every current volume Enters on the new operator through the game's own sequence.
//
// A hook area (EndNavAreaHookShot) is entered through the same diff: its enter writes the "Search (RS)" row on the
// command controller in EndUISystem[19] and its leave clears it (RE-hookshot-two.md §1 item 5 (ii)). The component
// diffs the seat's own leader, so every enter or leave it runs is the seat's: the seat's command controller is put in
// that slot on its own, whether or not the other three held (a scene that gives the member back unbinds the operator
// and leader, not the command controller), and the tick runs either way. Known edge: a seat that stands in a hook area
// before its command controller is bound (the first frame of a join) writes the row on player 1's menu, and the later
// leave, then the seat's, does not clear it there.
//
// Rooms per player (research/RE-room-walk-limit.md; the user's report of 2026-10-26: player 2 in the item shop left
// player 1 walking everywhere, player 2 never capped). An InDoor volume's Enter, Stay (every frame) and Leave write the
// room's walk limit onto the default party leader, whichever controller's component runs them, so a seat's room capped
// player 1's character and never the seat's. The three handlers' leader calls are redirected: while a seat's component
// runs the game's diff (its tick, the spawn re-enter), keyed on the component's owner rather than the scopes, or the
// game's camera reset diffs the primary controller's component while a seat's Primary scope makes it the seat's (any
// game code run in the seat's scopes), they get that controller's own field character (the one the
// diff projected into the room; none read: nothing written); otherwise the game's answer. The game's handlers write the game's values. A member taken or given back has its room
// limit cleared to the game's "no room" value (a seat's controller ends with no Leave); its component writes it again
// while it stands in a room. Placed once from the game thread (Tick; the Leave call crosses a block), all three or
// none. Logged: Info at the placing, Debug at each Enter and Leave and each clear, Trace per Stay; the reading counts.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::nav_area_two {

/// Replaces the component's tick slot; registers the reading `nav-area-two`. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The seat's operator was just spawned, owned and bound in its scopes: the volumes its controller's component
/// already lists enter again on the new operator.
void OnOperatorStarted(int seat, void* seatController) noexcept;

/// Game thread, each engine tick: places the room handlers' leader calls once.
void Tick() noexcept;

/// Game thread, each engine tick: the seat's member while its player is in (null otherwise).
void TickSeat(int seat, void* member) noexcept;

}  // namespace lss::players::nav_area_two
