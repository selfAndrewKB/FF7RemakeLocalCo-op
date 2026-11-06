#pragma once

// Each seat's own PlayerTrigger: a copy of the game's one placed trigger actor (research/RE-player-trigger.md).
//
// The game has exactly one PlayerTrigger, a plain Actor placed in the party level with a capsule and an
// EndCapsuleOverlapComponent, handed to the primary controller's InitPartyLevel and attached to that controller's
// leader at every leader change. Encounter boxes, story triggers, field-action spots and the navi map's areas are
// swept through it, and only the primary controller's is swept. A seat that borrows player 1's trigger carries it on
// its own member: the game then starts encounters where that player walks and never where player 1 does.
//
// No game path makes a second trigger, so this is a documented recreation, the smallest one: the placed actor
// spawned again as a template (the engine's own spawn with the placed actor as Template duplicates its instanced
// components and tag), checked to carry its own root, capsule and overlap component, and given to the seat's
// controller through the game's InitPartyLevel. The game never sweeps a non-primary controller's trigger, so the
// copy is inert: player 1's trigger stays on player 1's leader, and a seat fires only what its character's own
// component does. One copy per seat, destroyed when that seat leaves. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::players::player_trigger {

bool Install(const platform::GameImage& image) noexcept;

/// The seat's trigger, spawned from player 1's if there is none yet; null (logged) if it could not be made or does
/// not carry its own components.
void* Ensure(int seat, void* playerOneController) noexcept;

/// Destroys the seat's trigger (if it still exists) and forgets it.
void Destroy(int seat) noexcept;

/// The trigger `actor` is this seat's own copy (not player 1's placed one, nor another seat's).
bool OwnedBy(int seat, const void* actor) noexcept;

}  // namespace lss::players::player_trigger
