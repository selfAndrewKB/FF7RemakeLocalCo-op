#pragma once

// A further player's member runs a walk-up action (ladder, squeeze, crawl, monkey bars, climb, jump, vault, slides,
// balance beam and the rest of world/traversal_walkup.h's list) as player 1's does: on its own pad, with its player's
// own cameras (research/RE-traversal-action-two.md; the start:
// world/traversal_walkup.h).
//
// The game runs every character's field action from one tick function registered to run on any thread, so in practice
// on a worker, where none of the mod's scopes reach. There its "is this the player?" tests (the component's type
// against the party manager's leader type, inline in every state) say no for a further player's member, so the game
// runs the member's version: it climbs to the far end by itself, with no input and no camera. Rebirth answered such
// tests per thread through its one leader-test getter (climb_context, leader_answer; cleanup-2026-10); Remake has no
// such getter, its tests are inline compares. So the mod uses the game's own placement instead: while a further player
// is in, the tick function's "run on any thread" flag is cleared (the engine reads it every frame and runs the tick on
// the game thread, as it does whenever concurrent ticks are off; set back when the last further player leaves), and
// the call that ticks one character's component runs, for a seat's member, inside the seat's Leader, Primary and
// Operator scopes with its pad's stick and buttons, and player 1's camera-cut stick latch cleared. The game's tests
// then answer for the seat, its input reads the seat's pad, and its camera calls land on the seat's operator.
// The slide-down's side steer runs in its state's post-tick, which the engine calls outside the component tick (at the
// top of the walk-up start loop, game thread): for a seat's member that call runs the same way.
// The balance beam's end delegate (animation side) tests player 1's camera for the flavor its start stored and clears
// that store; for a seat's member the mod keeps it out of that test, so the state's exit eases the seat's camera out.
// The squeeze's and the crawl's near-exit delegates (animation side, signalled by the behaviour partway along) start the
// leader's duct camera leave; for a seat's member their leader test says no, and a squeeze's camera hold would turn it to
// the member's facing (a 90-degree turn) until the squeeze ends. The mod notes the call and starts the seat's leave with
// the game's own function on the game thread the next tick (the crawl's through its stub's tail jump: its own first
// instruction is too short to redirect).
//
// What a leader's action does for everyone stays player 1's (the user, 2026-10-24: a crawl locks only the mover):
//   - the action start's global switches (the field menu, every walk-up start, encounters) are taken with player 1's
//     leader in place for a seat's member, so a seat's action takes none of them;
//   - the crawl's and the squeeze's field control lock is skipped for a seat's member (it would hand the scoped leader
//     to its AI and lock the field for player 1);
//   - player 1's own crawl or squeeze lock hands only player 1's character to its AI: the session asks MoverLockNow and
//     keeps the seats' members theirs; and that action's "no new action" switch is taken away for the seats' start
//     tests (world/traversal_walkup.h), a script's own hold kept.
// A seat's member is ticked in its scopes only once the four lock calls are redirected (the frame after the tick is
// first seen on the game thread): until then it runs as the game's member does. The lock redirects stay in place at an
// unload (three cross a 16-byte block and may only change on the thread that runs them; they pass straight through when
// no seat's component ticks).
// Known gaps (reviews 2026-10-24): player 1's crawl lock is in every leader test's lock mask, so while player 1 crawls a
// seat's ladder climb runs as the member's version; in the frame or two after a join or a level change, while the
// tick is still on a worker, player 1's crawl lock comes from its game-thread task (not redirected) and hands the
// seats' members to their AI until its unlock; a story lock taken during player 1's crawl or squeeze is not a first
// lock, so the seats' members stay playable through it.
// The ladder's camera exit and the squeeze's and crawl's final switch-off run on the animation side, outside these
// scopes, and test the leader there: for a seat's member they never run, so when its action ends the mod leaves what is
// left on the seat's operator with the game's own leave (the duct camera, eased; skipped if already leaving) or
// deactivate (logged). Not covered yet: the ladder camera's mid-climb phase changes on the animation side, and a two-way
// jump's direction (the game's default). The gap jump is not admitted yet: its long jump's finish ticks the component
// from the animation side, outside these scopes (research/RE-traversal-slides-two.md).
// Logged: Info when the tick moves; Debug per seat action start and end, per camera eased out (near the exit or after the
// action) or switched off, and the squeeze-camera watch (any player's duct camera through the first 3 s of a squeeze,
// research/RE-traversal-duct-camera-two.md); in the `traversal.action` reading the tick's thread counts and the time
// spent inside the component ticks per frame over the last 5 s.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::traversal_action {

/// Checks every site against the research and places the component-tick redirect, the action start's entry, the
/// slide-down's post-tick, the balance beam's end delegate and the squeeze's and the crawl's near-exit delegates (logged;
/// the last four only warn when they fail). False if a check failed: further players' members then take walk-up actions as the game's members do.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick: the seat's member while its player is in (null otherwise).
void TickSeat(int seat, void* member) noexcept;

/// Game thread, each engine tick after the seats: `anyIn` while a further player is in. Moves the field-action tick
/// to the game thread and back, places the lock calls once it runs there, and logs the seats' action changes.
void Tick(bool anyIn) noexcept;

void Remove() noexcept;

/// The field-action tick has run on the game thread since the mod moved it (any thread).
bool OnGameThread() noexcept;

/// A seat's member's component ticks in its scopes now: the tick runs on the game thread and the lock calls are
/// redirected (any thread). Until then a seat's walk-up start would run the game's member version.
bool Scoping() noexcept;

/// The field control lock being taken now is player 1's own crawl's or squeeze's (it locks only that player): inside
/// its crawl lock call, or while its squeeze's queued lock stands. Game thread.
bool MoverLockNow() noexcept;

/// Player 1's leader's running action took the "no new action" switch for everyone (one count of it). Game thread.
bool PlayerOneActionHoldsNoAction() noexcept;

/// The `traversal.action` reading.
std::wstring Reading() noexcept;

}  // namespace lss::world::traversal_action
