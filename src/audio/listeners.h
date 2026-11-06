#pragma once

// Every player hears the sounds near them: each SEAD sound is measured from the nearest player's listener
// (research/RE-audio-listener.md, plan/topics/audio.md).
//
// SEAD (Square Enix's audio middleware) measures every placed or moving sound from one listener object, which the SEAD
// audio device sets each audio frame from the engine's listener [0], player 1's view; the engine itself keeps a
// listener per view (its viewport's Draw sets one per local player with a view, in column order, seen live). A
// sound farther than its range from that one listener was never voiced, however near a further player it played, and
// AutoSe's footstep clip and its rustle and swing range were measured from it too. Every lever runs the game's own
// code and changes only which listener it reads:
//   - the getter (every SEAD reader of the listener calls SeadLayListener_Get) answers a per-thread listener while the
//     mod has set one, the game's singleton otherwise: only the call the mod wraps sees another player's listener;
//   - a listener per further seat: a copy of the game's listener object, set every SEAD frame by the game's own setter
//     from the engine's listener at that seat's column (read where the device pushes [0]), then given its velocity by
//     the game's own update, on SEAD's thread;
//   - moving, placed and wedge sounds: the audibility test takes the nearest listener (a sound keeps its listener
//     while within its range until another is 20 % nearer, so its pan does not flip between two players), and the
//     sound's update (doppler, gain, pan, the voice's start and stop) runs from the listener its test chose. One voice
//     per sound, as before;
//     sounds near a further player play in the shared mix, panned from that player's camera;
//   - AutoSe (the footstep and cloth detection, game thread): the distance clip cuts a character only when the game's
//     clip answers "beyond" from every player's listener; the rustle and swing range is measured from the nearest one.
// Unchanged: area sounds (polygon, sector: their test measures a shape's point nearest the listener, logged only),
// ambient and direction sounds (no place to be near), listener-relative moving sounds, the obstruction pass and the
// singleton itself (player 1's mix). A seat's listener is on while its player is in, its view has a column, and the
// engine's listener at that column follows the seat's own camera (an entry the engine stopped updating turns it off).
// Nothing is formatted, locked or logged off the game thread: the audio and SEAD threads write atomics and seqlocks.
// Logged: the install, each seat's listener on or off with the reason, SEAD setting it or not, the engine's listener
// count (Info); each hook's thread once (Debug); a summary every 10 s (Debug): per sound class the sounds measured from
// each listener and heard, and those cut while within another player's reach (the check that the fix works: about 0);
// the updates per listener; the clip's cuts each seat's listener kept; the analyses measured from a seat. A line per
// tick with the listeners (Trace). Reading `sound`: the last summary.

namespace lss::platform {
class GameImage;
}

namespace lss::audio::listeners {

/// Checks and places each hook (each optional; logged). False if none could be placed.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread, each engine tick: the seat's controller while its player is in (null otherwise).
void TickSeat(int seat, void* controller) noexcept;

/// Game thread, each engine tick after the seats: the columns the players and the mirrors fill.
void Tick(int viewColumns) noexcept;

}  // namespace lss::audio::listeners
