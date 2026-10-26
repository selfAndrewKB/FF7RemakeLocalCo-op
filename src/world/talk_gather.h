#pragma once

// Player 1's character is brought to a talk another player starts, when it is far (research/RE-talk-far.md; the user,
// 2026-10-26: "Are they teleported to said conversation when Cloud has to speak? That would be preferred.").
//
// A further player's NPC talk plays as the game scripts it (world/field_targets): its speaker and listeners are named
// by the data, so player 1's character (Cloud) speaks wherever it stands, and the talk camera frames it there. Nothing
// in the game gates a talk on distance or brings the speaker over (RE §2). So, with the game's own calls, in the order
// the game makes them after its own leader teleports:
//   - the act channel ticking is noted around the act tick's two calls (call redirects), so the game's hand of its
//     leader to its AI (heard by players/party_events) knows which act took the field lock;
//   - at that hand, when the act's object is an NPC a further player pressed (field_targets::PeekTalkPress), the act is
//     not a shop (a show-menu act is the opener's alone), and player 1's character stands more than 8 m (flat) from the
//     player who pressed and runs no field action (a ladder, a crawl, the hook): a navmesh point 3 m from the presser,
//     toward where player 1's character stood or else to either side (the game's own navmesh query, with the shape the
//     game's catch-up teleport uses), then the game's battle-warp move on player 1's character (stops its AI move, the
//     party teleport that syncs its trigger, nav area and environment, a short fade-in), facing the presser, then the
//     game's field camera reset behind the leader (player 1's view snaps behind it before the talk camera blends in).
//     Decided once per press (moved, near, busy or no point): a later act of the same talk never moves it. (The research measured player 1's character
//     to the NPC against the NPC's reaction range; the presser stands within that range, so 8 m from the presser is
//     the same test with the game's own "far" value.)
// Outside any player's scope (they read "the leader" and the primary controller). Nothing when no navmesh point is
// found (the talk plays as before, logged). Player 1's character stays where it was brought. Logged: install (Info),
// each move (Info), a near or failed case (Debug); reading `talk-gather`. Game thread.

namespace lss::platform {
class GameImage;
}

namespace lss::world::talk_gather {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The game's leader was just handed to its AI (players/party_events, after the game's hand). Game thread.
void OnLeaderHanded() noexcept;

}  // namespace lss::world::talk_gather
