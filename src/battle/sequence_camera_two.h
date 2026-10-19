#pragma once

// The battle sequence cameras (abilities, enemy specials, summons) on each player's own camera
// (RE-seq-camera-two.md; Rebirth's player_two/attack_cameras, the same leak with a different state set).
//
// A battle camera sequence is one level-sequence actor with one camera actor; the request list keeps them, and
// each field camera operator's tick evaluates every entry against its own leader (per player since v0.16.0) and
// takes the last active one as its view. What the game keeps once, because single player has one camera:
//   - the camera actor's latched points (its "fix" latches and the fixed aim, eye and input-offset points): the
//     first operator to evaluate a shot after a cut fixes them for both views, from its own camera position;
//   - the sequence actor's blend pose (captured from the primary camera manager at a cut, a jump or the stop:
//     player 1's camera), its cut one-shot (consumed by the first evaluator) and its blend-out delta (latched by the
//     first evaluator): a blended ability camera on player 2's half started from, and returned through, player 1's
//     camera, and its cut landed on player 1's view;
//   - the world state the evaluation's tail writes (the others' game-speed slot, the owner's forced LOD, the no-fade
//     list) and each entry's active byte read by the next field step: the operator that ticks last decided.
// Levers, the game's own code throughout:
//   - player 2's copy of the camera actor's history block and the sequence actor's blend, cut and delta fields is
//     swapped in around player 2's operator's evaluation of each entry (the game's evaluation then latches, blends
//     and cuts on player 2's own state); the copy's blend pose is player 2's camera manager's cached view, read
//     the way the game reads player 1's, when the game captures (a cut, a jump) and at player 2's first evaluation
//     after a stop; the latches start zero, as the game zeroes the live ones at a cut;
//   - the cut raised in player 2's tick goes to the game's own primary-scoped cut (player 2's camera manager);
//   - the tail's world writes follow "active on either camera": the operator that evaluates second in a frame is
//     seeded with the first's active camera, so the game's tail applies the union with its own code, and each
//     entry's active byte is the OR of both operators' after both ticked.
// Not covered: sequence shakes start on player 1's camera manager only (no game path: a recreation, the user's
// call), and the Command/LeaderChange flavors' own evaluation of the last active entry (player 1's block).
// Game thread only (the operator ticks and the sequence actors' ticks).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::sequence_camera_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Each engine tick: the frame's start (the first operator to evaluate is not known yet) and the straddling cut
/// redirect placed once from the game thread.
void Tick() noexcept;

/// Right after operator `player`'s tick (1 player 1's, 2 player 2's): its view of each entry's active byte recorded,
/// and the OR written once both have ticked this frame.
void AfterOperatorTick(int player) noexcept;

}  // namespace lss::battle::sequence_camera_two
