#pragma once

// The game's read through a character's referenced actor, guarded where that actor is not an End actor
// (research/RE-referenced-actor-crash.md).
//
// Every End character carries a reference block whose first field names another actor as a weak object reference
// [what it names: UNKNOWN]. The End actor interface's state read resolves that reference, casts the object to the
// interface (the stock UObject::GetInterfaceAddress) and asks the result for a byte, with no test of the cast. The
// game itself reaches the read through its primary controller alone (its trigger sweep asks the party leader before
// each sweep), where the referenced actor is always an End actor. A seat's member can hold a reference to another
// kind of actor at a minigame's end, and the seat's own sweep (world/traversal_walkup.h) then reaches the read with
// it: the game dereferences null and crashes.
//
// The read's entry is redirected to the same tests the game makes before its dereference: the reference resolved by
// the engine's rule (players/weak_object.h), the object cast with the game's own function and class. A reference
// that resolves to nothing, or to an object without the interface, answers "no" (0) instead of the crash; every
// other call runs the game's own read, so no answer the game gives itself changes. Logged at Info the first time
// each class of such an object is met, with the class's name; a count at Debug after. Reading `referenced-actor`.
// Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::world::referenced_actor {

/// Checks the read's and the cast's entry bytes and redirects the read (logged). False if a check failed: the game's
/// read then runs unguarded, as without the mod.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::world::referenced_actor
