#pragma once

// A watch on the game's sound listener (research/RE-audio-listener.md §7): logging only, before the fix that gives each
// further player a listener of their own (plan/topics/audio.md).
//
// SEAD (Square Enix's audio middleware) measures every placed or moving sound from one listener object. The SEAD audio
// device sets it each audio frame from the engine's listener [0], player 1's view; the engine itself keeps a listener
// per view (the viewport client's Draw sets one per local player with a view, mirrors included). A sound farther than
// its range from that one listener is cut, however near a further player it plays. Footsteps also pass AutoSe's
// distance clip and, for rustle and swing sounds, its range compare, both measured from that listener.
//
// Every hook calls the game's own function first and changes nothing; it then counts:
//   - game thread, each tick: the engine's game-thread listeners, each seat's column (its view's place in the engine's
//     list, which is its listener's index if the engine numbers listeners as its views; not its screen, which the
//     column order may move) and its controller's own listener
//     position (the call Draw makes) against that entry, and each mirror column's entry against player 1's;
//   - audio thread, the device's push: the engine's audio-thread listeners and the position the push sent SEAD (and
//     whether it sent an override instead of the engine's first listener);
//   - SEAD's thread, the five sound classes' audibility tests: per class the sounds checked, heard from player 1,
//     listener-relative, and those within a further player's reach (their range from that player's listener) with,
//     of those, the ones player 1's listener cut. Approximate: the game pulls one axis of a moving or placed
//     sound's place (its y) toward the listener's before measuring (the watch takes the source as placed), and measures an area sound from
//     its point nearest player 1;
//   - the sounds started (the engine sound's SEAD start), which a fix must not change;
//   - AutoSe: clip reads, cuts for player 1, and cuts a further player's listener would have kept; the rustle and
//     swing analyses where a further player is nearer; footsteps from animation notifies against AutoSe's step events.
// Each hook's thread is logged once, and a hook seen on a second thread once more (the seqlocks assume one writer).
// Nothing is formatted, locked or logged off the game thread: the other threads write atomics and seqlocks
// (platform/seqlock.h). A seat's column is read from the engine's listener of that index even in a single-view moment
// (a menu, a scene), when the engine stops updating it: the counts then measure from where that view last was.
// Logged: the install (Info); a seat's column or the engine's listener count changing, each hook's thread once, and a
// summary every 10 s (Debug); a line per tick with the listeners (Trace). Reading `sound`: the last summary.

namespace lss::platform {
class GameImage;
}

namespace lss::audio::listener_watch {

/// Checks and places each hook (each optional; logged). False if none could be placed.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread, each engine tick: the seat's controller while its player is in (null otherwise).
void TickSeat(int seat, void* controller) noexcept;

/// Game thread, each engine tick after the seats: the columns the players and the mirrors fill.
void Tick(int viewColumns) noexcept;

}  // namespace lss::audio::listener_watch
