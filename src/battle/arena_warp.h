#pragma once

// A further player's character brought into the fight on a double click of the left stick: a character can be locked
// outside a fight whose only way in is a crawlspace, which the game closes while the fight lasts
// (research/RE-arena-warp.md).
//
// The game has an in-battle recovery warp for one member: its revive and its leader switch call it for a character
// outside the battle area (a navmesh point along the party leader's facing, then the battle warp with its fade-in).
// The mod calls it for a further player's member, with the leader switch's gates, when that player clicks L3 twice
// within 0.4 s in a battle. L3 is the game's Dash, which does nothing in battle; the press is the game's, read with its
// own getter inside the player's button scope (players/pad_buttons.h), at the player's command menu tick: so only with
// the player's own pad (never player 1's), with `command-menu` on and the player's command menu set up. The warp runs on
// the next engine tick, outside every player's scope (the warp reads "the party leader": player 1's character):
//   - in a battle, the battle system's no-recovery byte clear, its ready word set (the game's own gates);
//   - the member alive (not KO: the game's gate); not locked in an action (the leader switch's "may be left" rule) and
//     in no field action (the game would cut it short; refused instead): the mod's own stricter guards;
//   - not paused, no menu up, no scripted event holding the party;
//   - outside the battle area by the game's own test; inside it nothing moves ("already in the fight").
// The distance is the game's GameParam BattleTeleportNavDistanceChangeCharacter, else its default 3 m. Player 1's
// character is the party leader the area is built around: its double click does nothing. Logged: Info at install and
// at each answered double click (the player's own ask: brought in, already inside, refused and why, nothing moved);
// the reading `arena-warp` (with the presses seen). Game thread.
//
// Outside a battle the same double click asks for the field's answer: the character brought back beside player 1's
// (world/party_warp.h). The press is read here either way, so one reader counts a pair of clicks once.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::arena_warp {

bool Install(const platform::GameImage& image) noexcept;

/// Game thread, inside the seat's command menu tick with its scopes and button scope held: reads its Dash press.
void ReadPress(int seat) noexcept;

/// Game thread, each engine tick, outside every scope: runs an asked warp (in a battle), or passes the ask to
/// world/party_warp (outside one).
void Tick() noexcept;

/// The seat left: its clicks are forgotten.
void Forget(int seat) noexcept;

}  // namespace lss::battle::arena_warp
