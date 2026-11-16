#pragma once

// A further player's member never fires its own story box ahead of the story, and never loses one: no line of dialogue
// is ever cut or lost (research/RE-held-member-story-boxes.md, RE-tifa-barret-talk-order.md: in the Ch. 5 tunnel a
// player who walks Tifa into her talk box with Barret before her step listens for the talk's trigger stalls the scene
// for good).
//
// Each character's own body is swept for the level's group boxes every tick, whoever controls it; a box limited to
// one member (its group does not list the party leader) is placed where that member arrives when the level moves it or
// when it follows its leader in. A member its player walks ahead reaches such a box early. The box's enter
// (EndFieldStateTrigger_OnColliderBeginOverlap) is redirected: for a member its player holds, on a move the level did
// not send, a member-only box's enter is not called now but remembered (the game then records nothing; its own end
// handler is safe with no record). The member then enters it "now", by the game's own enter called with the same
// three values, the moment a companion would in single player:
//   - the level moves it or hands it to its AI (its PLAYER slot empties: a guide, a script, a scene's hand), or writes a
//     move of its own on it (a run or a lead, players/lead_gate OnLevelMove), even one its player keeps: in single player
//     that move carries the companion through its boxes (the Ch. 5 dash crosses the talk box and the fence box in about
//     3 s, before the talk sends "head for the stairs"; counting only a move kept at a marked point, the
//     fence box, held until player 1 comes half a minute later, would miss that trigger and the scene would stall);
//   - its player leaves (it follows on its AI);
//   - player 1's character comes within a few metres of the box (world/story_box_rules.h Reaches): a following companion
//     walks in about then.
// Not during a cutscene (the game's own sweep dispatches nothing in the CINEMA mode). If the member is no longer inside
// at that moment, the game's end handler follows at once, so the box's occupant record stays balanced. Party boxes
// (turn-backs, scene starts), group-less boxes, doors and every AI-driven member are the game's.
// Info lines: each enter held back, each replay (and whether the box was off then: nothing sent, as in single player),
// and each remembered enter whose box went away first (its level unloaded): the only way one is dropped.
// Game thread only: the enter runs inside the world tick's dispatcher; Tick runs outside it, where the redirect is placed.

#include "world/story_box_rules.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::world::story_boxes {

/// Checks the enter and the game functions the replay and the reads call against the research, registers the
/// `story-box-hold` switch and the `world.story-boxes` reading; the redirect is placed by Tick. False (logged) on a
/// failed check: the boxes are then the game's, as before.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The layout object `object` is a member-only story box (its group lists `member`'s tag and not the party leader's,
/// as the enter tests) whose trigger state names `trigger` (FName bits: what it sends) and is switched on, so `member`
/// walking in sends `trigger`. False when not installed or unread. players/lead_gate asks it of a level run's way out
/// (OwnBoxAhead). Game thread.
bool ArmedOwnBox(std::uint64_t object, const void* member, std::uint64_t trigger) noexcept;

/// The placed shape of the story box registered under `tag` (its layout node name). False when not installed, not
/// registered or not a box component.
bool BoxShape(std::uint64_t tag, story_box_rules::Box& out) noexcept;

/// `member` was brought back for the level's run from `from` to `to` (world/run_catch_up): each of its waiting enters
/// whose box that straight run passes through waits for the member's own body instead of firing at its AI's hand
/// (story_box_rules Next). Returns how many. Game thread.
int AwaitRun(const void* member, const float from[3], const float to[3]) noexcept;

/// Game thread, each engine tick, per built seat: the seat's member while its player is in (null otherwise).
void TickSeat(int seat, void* member) noexcept;

/// Game thread, each engine tick outside the world tick, after TickSeat: places the redirect on the first call, then
/// replays or drops the remembered enters (`pawnOne`: player 1's character, null when unread).
void Tick(const void* pawnOne) noexcept;

}  // namespace lss::world::story_boxes
