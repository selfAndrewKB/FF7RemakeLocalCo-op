#pragma once

// A further player's member runs a walk-up action (ladder, squeeze, crawl, monkey bars, climb, jump, vault, slides,
// balance beam and the rest of world/traversal_walkup.h's list) as player 1's does: on its own pad, with its player's
// own cameras (research/RE-traversal-action-two.md; the start:
// world/traversal_walkup.h).
//
// The game runs every character's field action from one tick function registered to run on any thread, so in practice
// on a worker, where none of the mod's scopes reach. There its "is this the player?" tests (the component's type
// against the party manager's leader type, inline in every state) say no for a further player's member, so the game
// runs the member's version: it climbs to the far end by itself, with no input and no camera. The tests are inline
// compares, with no getter to answer per thread, so the mod uses the game's own placement: while a further player
// is in, the tick function's "run on any thread" flag is cleared (the engine reads it every frame and runs the tick on
// the game thread, as it does whenever concurrent ticks are off; set back when the last further player leaves), and
// the call that ticks one character's component runs, for a seat's member, inside the seat's Leader, Primary and
// Operator scopes with its pad's stick and buttons, and player 1's camera-cut stick latch cleared. The game's tests
// then answer for the seat, its input reads the seat's pad, and its camera calls land on the seat's operator.
// The slide-down's side steer runs in its state's post-tick, which the engine calls outside the component tick (at the
// top of the walk-up start loop, game thread): for a seat's member that call runs the same way.
// The action start runs the new state's Start and the old state's Exit inside it, where a seat's member has player 1's
// leader in place (below), so the leader-gated camera code in them never ran for a seat. The monkey bars' Start and Exit
// and the balance beam's Start (camera flavor 20, the spot's own camera by name) are replaced in their states' vtables:
// called by that action start for a seat's member, they run with the seat's leader again for the one call, so the camera
// switches on and eases out on the seat's operator as player 1's does on his, and the monkey bars' in-use markers aim from
// the seat's member (their user, as in single player). The monkey bars' Exit's base exit (its end signals: the leader's
// set or the member's) runs with player 1's leader again, so a seat's action still sends the member's. After a monkey
// bars, beam or wire-fence action the flavor-20 camera is left to finish its leave, else switched off.
// The balance beam's end delegate (animation side) tests player 1's camera for the flavor its start stored and clears
// that store; for a seat's member the mod keeps it out of that test, so the state's exit eases the seat's camera out.
// The squeeze's and the crawl's near-exit delegates (animation side, signalled by the behaviour partway along) start the
// leader's duct camera leave; for a seat's member their leader test says no, and a squeeze's camera hold would turn it to
// the member's facing (a 90-degree turn) until the squeeze ends. The mod notes the call and starts the seat's leave with
// the game's own function on the game thread the next tick (the crawl's through its stub's tail jump: its own first
// instruction is too short to redirect).
//
// A spot's start signals (sent from the action's state enters, for the party leader's character only) set level states
// the save keeps, and their end signals go from the state's Exit, which a seat's action runs with player 1's leader: a
// seat's member's start is therefore sent with player 1's leader too (the game's member branch), so the two match and a
// spot's state (such as a camera on player 1's camera manager) is never left on; a switch board keeps the leader's start,
// as its Exit sends the leader's end for the seat (research/RE-camera-slot14-stuck.md).
//
// What a leader's action does for everyone stays player 1's (a crawl locks only the mover):
//   - the action start's global switches (the field menu, every walk-up start, encounters) are taken with player 1's
//     leader in place for a seat's member, so a seat's action takes none of them;
//   - the crawl's, the squeeze's and the grappling hook's field control lock is skipped for a seat's member (it would
//     hand the scoped leader to its AI and lock the field for player 1);
//   - player 1's own crawl, squeeze or hookshot lock hands only player 1's character to its AI: the session asks
//     MoverLockNow and PlayerOneMoveHolds and keeps the seats' members theirs; the scene camera (views/scene_camera)
//     does not count its bits as a scene and the command menu (battle/command_menu) does not copy player 1's closed
//     menu to the seats while it holds; and that action's "no new action" switch is taken away for the seats' start
//     tests (world/traversal_walkup.h), a script's own hold kept;
//   - a seat's own action takes "no new action" for the seat's own target pick and start tests (NoActionScope), as the
//     game's start takes it for its leader: a second press cannot restart the seat's running action.
// A seat's member handed to its AI (a lead, a scene: the game's own hand) is an AI ally as in single player: between
// actions its component ticks outside the seat's scopes and the animation-side fixes leave it alone, so the tick's
// non-leader branch starts the nav-link actions its path asks for (the door a guide opens on a lead; inside the seat's
// Leader scope the member would pass the leader test and run into the closed door). Decided once per frame,
// so a hand or a give-back takes effect from the next frame; an action keeps the mode it started in, and the seat's
// camera clean-up skips an action its AI ran. The walk-up start and the field-target press refuse a handed member
// (the player does not hold it).
// A seat's member is ticked in its scopes only once the four lock calls are redirected (the frame after the tick is
// first seen on the game thread): until then it runs as the game's member does. The lock redirects stay in place at an
// unload (three cross a 16-byte block and may only change on the thread that runs them; they pass straight through when
// no seat's component ticks).
// Known gaps: player 1's crawl, squeeze and hookshot locks are in every leader test's
// lock mask, so while player 1 crawls, squeezes or turns to a hook point a seat's action start is refused and its
// ladder climb runs as the member's version; in the frame or two after a join or a level change, while the tick is
// still on a worker, player 1's crawl lock comes from its game-thread task (not redirected) and its hookshot lock is
// not seen as the mover's (its Set runs on the worker), and they hand the seats' members to their AI until the unlock; a
// story lock taken during player 1's crawl, squeeze or grappling-hook turn is not a first lock, so the seats' members are
// handed by the session's backstop (PlayerOneMoveHolds says no once the mask holds more than the move's bits).
// The ladder camera's leave at an exit (top, bottom, slide landing, jump off) is asked by the behaviour's phase delegate
// on the animation side, for the leader's type only: for a seat's member the call is noted (its stub's tail jump) and the
// game thread runs the same delegate in the seat's Leader and Operator scopes the next tick, so the seat's ladder camera
// eases out at the exit as player 1's does. The squeeze's and crawl's final switch-off also run on the animation side:
// when a seat's action ends the mod leaves what is left on the seat's operator with the game's own leave (the duct and
// ladder cameras, eased; skipped if already leaving) or deactivate (logged). A two-way jump takes its side from where
// the jumper stands (research/RE-traversal-phases-two.md).
// The three moves the game's AI companions also take (research/RE-traversal-deferred-two.md): the gap jump's long-jump
// finish ticks the component on the animation side, outside these scopes: for a seat's member it only sets the state
// done and the member's next scoped tick ends the jump; the pole slide's finish would leave player 1's pole camera (its
// "was the leader" byte was captured in the seat's scopes): for a seat's member the byte is cleared and the seat's own
// camera leave runs the next tick; the wire fence's Start and Exit run with the seat's leader again (its camera), the
// actor's "the party leader is on this mesh" byte the AI members follow set to what player 1's own climb makes it, and its end signals under the
// pause, as the fence Start's base start (its tail call takes the leader-typed OnOff 25, a global switch).
// Logged: Info when the tick moves; Debug per seat action start and end, per camera eased out (near or at the exit, or after
// the action) or switched off, and the squeeze-camera watch (any player's duct camera through the first 3 s of a squeeze,
// research/RE-traversal-duct-camera-two.md); in the `traversal.action` reading the tick's thread counts and the time
// spent inside the component ticks per frame over the last 5 s.

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::traversal_action {

/// Checks every site against the research and places the component-tick redirect, the action start's entry, the
/// slide-down's post-tick, the balance beam's end delegate, the squeeze's and the crawl's near-exit delegates, the
/// ladder's phase call, the gap jump's and the pole's finishes, the monkey bars', the beam's and the wire fence's
/// Start/Exit slots with their base and end-signal calls, and the grappling hook's three lock calls (logged; all but the
/// first two only warn when they fail). False if a check
/// failed: further players' members then take walk-up actions as the game's members do.
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

/// Player 1's own crawl, squeeze or grappling hook holds the field control lock now, or is taking it (it locks only that
/// player): inside its crawl lock call or while its crawl lock stands, or while its squeeze's or hookshot's queued lock
/// stands. Game thread.
bool MoverLockNow() noexcept;

/// Of `mask` (the field control lock mask read now), the bits that are player 1's own crawl, squeeze or grappling hook,
/// and a bit once seen as the move's kept while it stays set (its queued unlock drops the flag before the bit clears).
/// Kept up to date by the scene camera's read every frame (views/scene_camera). Game thread.
std::uint32_t PlayerOneMoveBitsIn(std::uint32_t mask) noexcept;

/// The lock `mask` holds nothing but player 1's own move (a mask still 0: MoverLockNow, its lock queued and not yet
/// processed). Game thread.
bool PlayerOneMoveHolds(std::uint32_t mask) noexcept;

/// A seat's member may use the grappling hook: its member's actions run in its scopes and the hookshot's lock calls are
/// redirected (any thread).
bool HookShotReady() noexcept;

/// The seat whose member's field-action component the game thread ticks now (its actions start and end inside that
/// tick: a grappling hook's flight among them); 0 for player 1's, for none, or off the game thread.
int ActingSeat() noexcept;

/// For one seat's run of the game's target pick or walk-up start (game thread; opened before the seat's Leader scope,
/// which would make player 1's leader read as the seat's member): the game's "no new action" switch (OnOff 4, a count)
/// as the seat's own play sees it. The seat's own running action's share is added, as the game's action start adds it
/// for its leader: the pick then dims the seat's targets and no start is admitted while the seat's member acts
/// (research/RE-hookshot-press.md: without it a second press restarts the running action, and a restarted grappling hook
/// loses its animations: a T-pose flight, no landing, the mesh left tilted). `withoutPlayerOne` also takes player 1's own
/// action's share away (it locks only the mover): the walk-up starts, which keep a copy of each
/// spot's user per seat; the pick keeps it (its targets have no per-seat user: player 1's door or hook point is not the
/// seat's to press meanwhile). Put back by difference when the scope closes. Logged: Debug when a seat's own share comes
/// and goes.
class NoActionScope
{
public:
    NoActionScope(int seat, bool withoutPlayerOne) noexcept;
    ~NoActionScope();
    NoActionScope(const NoActionScope&)            = delete;
    NoActionScope& operator=(const NoActionScope&) = delete;

private:
    std::int32_t* at_    = nullptr;
    std::int32_t  delta_ = 0;
};

/// The pole slide's finish fix is installed: a seat's pole slide may be admitted (world/traversal_walkup.h).
bool PoleReady() noexcept;

/// Some player's character is in a field action now: player 1's leader's component reads a running action, or a seat's
/// member ran one at its last tick. True too while player 1's component cannot be read or the module is not installed
/// (asked before a level state is judged at rest). Game thread.
bool AnyoneActing() noexcept;

/// The `traversal.action` reading.
std::wstring Reading() noexcept;

}  // namespace lss::world::traversal_action
