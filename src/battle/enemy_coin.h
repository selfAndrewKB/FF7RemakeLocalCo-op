#pragma once

// Enemies share their attention between the humans: one coin per enemy decision (RE-enemy-attention-coin.md; the
// user's rule B of 2026-10-20; Rebirth's player_two/target_coin and leader_reads, reshaped to Remake's one decision
// call and inlined leader tests).
//
// In the single-player game "the player" is one character: the enemy scripts aim at it (SetTargetByType(UCPC),
// GetLeader, the target collectors, the expectation counts, IsAIPC, the party API), the hate rows weight it twice an
// AI ally and win ties for it, the enemy EQS keeps it, and its opening target is the leader. In co-op every one of
// those named player 1's character, so enemies chased Cloud and treated Aerith as the AI ally (the user's reports:
// Rude on Cloud; an enemy circling Barret without attacking). The co-op result is the single-player one for both:
//   - the toss: at the game's own decision call (the battle AI tick's gate, true only while no action runs; the
//     mines' override too) each enemy whose last coin has seen an action tosses again, before the gate's hate
//     update and the script's OnUpdate: one coin per decision, held through the action by the game's own target lock
//     (a coin naming a downed human is answered by the other); the opening target tosses the first coin;
//   - the answers: the script natives that name the player run with the calling enemy recorded on this thread
//     (their UFunction native pointers swapped), and the reads inside them (three leader reads, the two expectation
//     counts' type reads, the party API's two results) answer the coin's human's member or type;
//   - rule B at the inlined "is this member the player" tests (the hate rows, the hate pick's tie-break, IsAIPC):
//     the party manager read there is answered by a block whose leader type is the member's own when the member is a
//     further human's (battle/type_answer_code.h), so both humans get the player's rows and the AI ally keeps its
//     single-player ratio; ties between humans go to the first in party order (the game's own rule with both "the
//     leader");
//   - membership: the enemy EQS target context keeps a human's member (its controller answers as a battle AI's);
//   - hits and reaction sensing by or on a further human's member run in that human's Leader and Primary scopes
//     (the counters aim at who hit; the victim's own camera shakes and pad rumbles; the hit stop as for the player).
// Nothing is written to the game's state: every answer passes through the game's own call. Any thread for the toss
// and the type answers (the AI tick is written to run off the game thread); the natives run on the game thread;
// hits and sensing are scoped on the game thread only (else counted and left as the game's).
// Logged: the install (Info), each toss (Debug), a 60 s summary (Debug), each answer (Trace). `enemy-coin` reads
// the counts; `coin-two` turns it off (every lever then passes the game's own answer through).

namespace lss::platform {
class GameImage;
}

namespace lss::battle::enemy_coin {

/// Places every lever (logged per lever; one that cannot be placed is left out and its reads stay the game's).
/// Needs players/scopes. At on_unreal_init (the crossing calls' rule: no battle exists yet).
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Game thread, once per frame: the seats' members, the native swaps until they are in place, the summary.
void Tick() noexcept;

}  // namespace lss::battle::enemy_coin
