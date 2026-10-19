#pragma once

// Player 2's own PlayerTrigger: a copy of the game's one placed trigger actor (RE-player-trigger.md §4-§5, Fix A).
//
// The game has exactly one PlayerTrigger, a plain Actor placed in the party level with a capsule and an
// EndCapsuleOverlapComponent, handed to the primary controller's InitPartyLevel and attached to that controller's
// leader at every leader change. Encounter boxes, story triggers, field-action spots and the navi map's areas are
// swept through it, and only the primary controller's is swept. The first seat (v0.3.0-v0.14.0) borrowed player
// 1's trigger for player 2, so it rode on player 2's member: the game then started encounters where player 2
// walked and never where player 1 did (the user's report, 2026-10-19).
//
// No game path makes a second trigger, so this is a documented recreation, the smallest one: the placed actor
// spawned again as a template (the engine's own spawn with the placed actor as Template duplicates its instanced
// components and tag), checked to carry its own root, capsule and overlap component, and given to player 2's
// controller through the game's InitPartyLevel. The game never sweeps a non-primary controller's trigger, so the
// copy is inert: player 1's trigger stays on player 1's leader, and player 2 fires only what its character's own
// component does. Destroyed when player 2 leaves. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::players::player_trigger {

bool Install(const platform::GameImage& image) noexcept;

/// Player 2's trigger, spawned from player 1's if there is none yet; null (logged) if it could not be made or does
/// not carry its own components.
void* Ensure(void* playerOneController) noexcept;

/// Destroys player 2's trigger (if it still exists) and forgets it.
void Destroy() noexcept;

/// The trigger `actor` is player 2's own (not player 1's placed one).
bool Owns(const void* actor) noexcept;

}  // namespace lss::players::player_trigger
