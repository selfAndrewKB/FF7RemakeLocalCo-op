#pragma once

// One registry of each seat's game objects: its player controller, its field camera operator and the member it
// plays, kept as weak pairs (players/weak_object.h) so a destroyed or replaced object resolves to null wherever it is
// asked for. The session binds the controller at the join and forgets the seat at the leave; the party seat
// binds the member at the seating and at the release; the camera module binds the operator at its spawn and at its
// take. Every other module asks here: Controller(seat), Operator(seat), Member(seat), and SeatOf(object) for the
// replacements the game hands an object with no identity. The scopes keep their own copies for the contexts they
// swap in (those must be the raw values the game reads). Game thread only.

#include "players/weak_object.h"

#include <cstdint>

namespace lss::players::seat_objects {

/// The seat's controller, operator or member, or null to forget that one. Unbuilt seats are ignored.
void BindController(int seat, void* controller) noexcept;
void BindOperator(int seat, void* cameraOperator) noexcept;
void BindMember(int seat, void* member) noexcept;
/// Everything of the seat forgotten (the leave, the forget).
void Forget(int seat) noexcept;

/// The seat's objects, resolved now: null when unbound, destroyed or replaced.
void* Controller(int seat) noexcept;
void* Operator(int seat) noexcept;
void* Member(int seat) noexcept;
/// The seat's operator as the pair the game's root field holds (unset when unbound): for the scopes that write it.
weak_object::Weak OperatorWeak(int seat) noexcept;

/// The seat whose controller, operator or member is `object`, else 0 (player 1's objects are never a seat's).
int SeatOf(const void* object) noexcept;
/// The seat whose controller's camera manager is `manager`, else 0.
int SeatOfCameraManager(const void* manager) noexcept;

/// Some built seat's controller resolves.
bool AnySeatIn() noexcept;

}  // namespace lss::players::seat_objects
