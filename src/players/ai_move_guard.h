#pragma once

// A further player's member is never given an AI-only move while its player holds it (the user, 2026-10-29: Aerith's
// interest walk looped on a ladder for 5 minutes, "make sure that loop can never happen";
// research/RE-interest-ladder-loop.md §6.1, §7).
//
// Square Enix's AI model (FEndAIModel, char+0x820) runs on a player's character too: its intentions (the walk to an
// interest point, a bump on contact, a turn in a talk, a wall contact) ask EndAIModel_CanRequestBehavior before adding a
// behaviour. While the player's manual control holds the move slot (slot 0) the game answers no; manual control drops
// under any field control lock, and an accepted walk then starts with no AI controller and faults. The game's own
// answer while manual control holds is the guard's answer for a held further member: slot 0 is refused whenever its
// PLAYER slot holds a controller. Manual control adds its behaviours directly and never asks, the other slots are
// passed, and a refusal is the requesters' normal "slot busy" path (they retry or skip). A member handed to its AI
// (PLAYER slot empty) is asked as the game asks, so its AI walks as an AI ally's.
// With the guard in place an interest scene never takes a held member (the user, 2026-10-30; players/field_hold.h
// JudgedState judges the member by the request beneath it): the walk cannot start under the player.
// Entry redirect on the function (two instructions copied): placed by Tick from the game thread, outside the world
// tick, where no AI tick runs (a thread between the two copied instructions at the swap would land in the jump; keep
// the install there). Remove may run on any thread: every entry already goes through the jump, and the restore swaps
// the one block back. The replacement runs on any thread (the AI logic tick of an AI-owned character may run
// on a worker): it reads the members the game thread published and the PLAYER slot, guarded, and counts; the game
// thread logs the counts.

namespace lss::platform {
class GameImage;
}

namespace lss::players::ai_move_guard {

/// Checks the function against the research and registers the switch and the reading; the redirect is placed by Tick.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread, each engine tick outside the world tick: places the redirect on the first call; logs the refusals.
void Tick() noexcept;

/// Game thread, each engine tick, per built seat: the seat's member while its player is in (null otherwise).
void TickSeat(int seat, void* member) noexcept;

/// The guard is placed and on: an interest scene leaves a held member with its player (players/party_member JudgedState).
bool Active() noexcept;

}  // namespace lss::players::ai_move_guard
