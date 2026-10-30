#pragma once

// A further player's member never fires its own story box ahead of the story, and never loses one (the user, 2026-10-30:
// "no dialogue is ever cut and goes missing ... I want that seamless"; research/RE-held-member-story-boxes.md,
// RE-tifa-barret-talk-order.md: in the Ch. 5 tunnel player 2 walked Tifa into her talk box with Barret before her step
// listened for the talk's trigger, and the scene stalled for good).
//
// Each character's own body is swept for the level's group boxes every tick, whoever controls it; a box limited to
// one member (its group does not list the party leader) is placed where that member arrives when the level moves it or
// when it follows its leader in. A member its player walks ahead reaches such a box early. The box's enter
// (EndFieldStateTrigger_OnColliderBeginOverlap) is redirected: for a member its player holds, on a move the level did
// not send, a member-only box's enter is not called now but remembered (the game then records nothing; its own end
// handler is safe with no record). The member then enters it "now", by the game's own enter called with the same
// three values, the moment a companion would in single player:
//   - the level moves it or hands it to its AI (its PLAYER slot empties: a guide, a script, a scene's hand), or sends it
//     on a move its player keeps;
//   - its player leaves (it follows on its AI);
//   - player 1's character comes within a few metres of the box (world/story_box_rules.h Reaches): a following companion
//     walks in about then.
// Not during a cutscene (the game's own sweep dispatches nothing in the CINEMA mode). If the member is no longer inside
// at that moment, the game's end handler follows at once, so the box's occupant record stays balanced. Party boxes
// (turn-backs, scene starts), group-less boxes, doors and every AI-driven member are the game's, as before.
// Info lines: each enter held back, each replay (and whether the box was off then: nothing sent, as in single player),
// and each remembered enter whose box went away first (its level unloaded): the only way one is dropped.
// Game thread only: the enter runs inside the world tick's dispatcher; Tick runs outside it, where the redirect is placed.

namespace lss::platform {
class GameImage;
}

namespace lss::world::story_boxes {

/// Checks the enter and the game functions the replay and the reads call against the research, registers the
/// `story-box-hold` switch and the `world.story-boxes` reading; the redirect is placed by Tick. False (logged) on a
/// failed check: the boxes are then the game's, as before.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread, each engine tick, per built seat: the seat's member while its player is in (null otherwise).
void TickSeat(int seat, void* member) noexcept;

/// Game thread, each engine tick outside the world tick, after TickSeat: places the redirect on the first call, then
/// replays or drops the remembered enters (`pawnOne`: player 1's character, null when unread).
void Tick(const void* pawnOne) noexcept;

}  // namespace lss::world::story_boxes
