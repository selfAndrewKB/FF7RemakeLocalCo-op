#pragma once

// Each player's own grappling-hook search arrow: left alone, player 1's search arrow hides whenever another player's
// appears (research/RE-hookshot-arrows.md). One shared list hidden by owner cannot work: the game clears the whole
// list at every enter.
//
// The game keeps the search compass and its arrows in one object (EndUISystem[55]): a hook area's enter and its
// target-lost re-show first clear every arrow in it, its leave, focus and EndPlay fade every arrow out; the area actor
// keeps one in-area flag and one arrow pointer. So one player's enter wiped the other's arrow, and the arrow made for a
// further player was player 1's (made with the first local player) and showed in player 1's view.
//
// Each further player gets its own slot object, built with the game's allocator and the values the game's constructor
// writes (the one recreated piece: the game builds it inline, with no constructor to call), and its own copy of each hook
// area's flag and arrow. When one of the six area functions that touch them runs inside a seat's scopes (its nav-area
// tick, its target pick, its member's action tick), the seat's slot object is put into EndUISystem[55] and the seat's copy
// into the actor for that one call, and both are put back after it; the compass and arrow the call creates are owned by
// the seat's local player (world/target_icons.h CreatingAs), and checked after. The game's own functions then run
// unchanged on the seat's objects: one player's enter, leave, focus or hookshot never touches another's. A seat's slot is
// ticked by the game's own slot tick once a frame, in the seat's own HUD pass after its view's geometry is set (else from
// the game tick), so its arrow turns toward the hook point from the seat's camera; hud/pass_hide.h shows each search
// widget only in its owner's view (OwnerSeat). An area's EndPlay clears every seat's arrow of it with the game's own
// clear, the game mode's reset clears the seats' slots where it clears player 1's, and a seat's slot is cleared when its
// player leaves or changes local player.
//
// A hook point used once shows no guide arrow again: every flight's start marks the area in use (facts::kFieldActionInUse),
// its enter and re-show make no arrow while it is marked, and the mark never clears before the level reloads
// (research/RE-hookshot-in-use.md). The mark is one per area, so player 1's flight alone would take the arrow from players
// 2 and 3. Each seat keeps its own copy of the mark beside its flag and arrow: a flight a seat's member starts (inside that
// seat's action tick, world/traversal_action.h ActingSeat) marks only the seat's copy, the area's own mark staying player
// 1's, and the seat's copy is put into the area for its enter and re-show. A switched-off area is left as it is for
// everyone. A seat's marks outlive its slot's clears and last until the area ends, as the area's own does. Player 1's
// slot, flags and arrows stay the game's own throughout; only in a
// mirror's own HUD pass are player 1's arrows placed again through the mirror's camera, and put back for player 1's pass
// (PlaceForMirror). Switch
// `search-arrows` (on: each player's own; off: the game's one arrow, as before). Game thread only.
// Logged: Info at install and at the switch; Debug per enter, leave, focus, re-show and EndPlay of a seat (the flag and
// arrow before and after, the seat's and player 1's arrow counts), per seat's flight that marks its copy, and when a
// seat's slot is made or cleared; Trace per
// stay; Warning when an arrow made for a seat is not owned by its player (cleared at once), when a seat's slot cannot be
// made (its calls then stay the game's, not retried) and when a put-back fails; the `world.search-arrows` reading.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::search_arrows {

/// Checks the seven area functions (the six above and the in-use mark), the slot's functions and the allocator, and
/// replaces the seven slots (logged; false unless all seven were placed, and then none is kept).
bool Install(const platform::GameImage& image) noexcept;

/// The slots back; on the game thread also the seats' arrows cleared and the reset call put back.
void Remove() noexcept;

/// Game thread, each engine tick: the seat's local player while its player is in (null otherwise).
void TickSeat(int seat, void* localPlayer) noexcept;

/// Game thread, each engine tick after TickSeat: the reset call placed once, and each seat's slot that no HUD pass ticked
/// since the last engine tick ticked here.
void Tick() noexcept;

/// Game thread, from the overlay driver after a further column's view geometry is written: that column's seat's slot
/// ticked (its arrows placed through its own player into this view).
void TickForPass(int seat) noexcept;

/// Game thread, from the overlay driver in a mirror's own HUD pass (hud/second_image.h), the column's host set in:
/// player 1's arrows placed through `controller` (the mirror's camera) by the game's own per-arrow placement alone
/// (EndUISlot55 PlaceArrow, its one controller lookup answered with the mirror's; RE-hookshot-arrows.md). It writes
/// only each arrow's angle and place: player 1's slot tick, which steps the slot's state, is never run again.
void PlaceForMirror(void* controller) noexcept;
/// Game thread, after the further passes with player 1's host set back: player 1's arrows placed through their own
/// player again, if a mirror placed them this frame.
void RestorePlayerOne() noexcept;

/// The seat whose search widget this is (1..), or 0 (player 1's, or not a seat's). Game thread.
int OwnerSeat(const void* widget) noexcept;

/// A seat's slot holds a compass or an arrow now. Game thread.
bool AnySeatArrows() noexcept;

/// The `world.search-arrows` reading.
std::wstring Reading() noexcept;

}  // namespace lss::world::search_arrows
