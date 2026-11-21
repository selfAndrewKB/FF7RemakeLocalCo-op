#pragma once

// A further player's character brought back beside player 1's in the field, when that player asks: a player can be left
// behind a door that shut, on the far side of a drop, or simply far away (research/RE-field-catchup-warp.md).
//
// In single player the game's follow AI warps a companion that has been more than 10 m from the leader for a while back
// beside it. A character a player holds never gets that warp: its AI does not run. So the player asks for it, with the
// press that brings a character into a fight (battle/arena_warp.h reads it: a double click of the left stick on that
// player's own pad, one press of the period key on the keyboard; outside a battle it asks here).
// The move is the game's own, in the order its catch-up warp makes it:
//   - the game's gates for that warp: no scene, talk or event holding the field, player 1's character with its player
//     and in no field action; and the mod's for a held character: it belongs to its player (no scripted hold), in the
//     field mode, in no field action of its own, and not already beside player 1's character (within 3 m on the same
//     level: a stray double click moves nothing); not paused, no menu up (world/party_warp_rules.h);
//   - a navmesh point 1.5 m from player 1's character: behind it, else to either side, else in front, never within 1 m
//     of another player's character (the game's query, the shape its field catch-up warps use; the query itself keeps
//     the point off every character, an AI companion included);
//   - the game's battle-warp move on the character there (its party teleport, which syncs the character's own trigger
//     sweep at the landing, and a short fade-in), facing as player 1's character does. Nothing else of the battle warp
//     (its control-layer bits can leave a field character unable to move). The character stays its player's: no AI move
//     runs on it, so nothing is cancelled or restarted. The player's camera follows the character to the new spot.
// Outside every player's scope (the move reads "the party leader": player 1's character). Player 1's own character is
// the one the others are brought to: its double click does nothing. Logged at Info: the install and every answered ask
// (brought back and from how far, or refused and why). Console: `party-warp <player>` asks as the double click does;
// reading `world.party-warp`. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::world::party_warp {

bool Install(const platform::GameImage& image) noexcept;

/// The seat's player asked to be brought back (battle/arena_warp's tick, outside a battle). Game thread, at the engine
/// tick, outside every player's scope. True when the ask was answered (brought back or refused, logged); false for a
/// seat that is none of the further players' or with the module not installed.
bool Bring(int seat) noexcept;

}  // namespace lss::world::party_warp
