#pragma once

// A scripted one-shot move the level writes on a further player's member (an act's event move, an event's placement:
// the game's FieldAI_RequestEventMove_L6) is preceded by the member's hand to its AI, as in single player, where the
// companion is its AI's when the level writes it (research/RE-scripted-entry-cut.md). The game writes the request
// without asking who holds the character, and a possession change after the write makes the game run the request's
// exit, which reports it finished at once: a hand after the write cuts a one-shot animation (chapter 13: Barret's entry
// into Aerith's house would end after 0.08 s, and the stay that waits for Cloud is set at once). So the writer is
// redirected: it finds the named character as the game does (world/named_actor: a dying one is left to the writer, which
// refuses it), asks the session to hand it when it is a seat's member its player holds (Session::HandBeforeScriptMove: a
// scripted hold of its own, the player's view kept), then runs the game's writer. The member comes back when nothing
// holds it, as after any script hold; player 1's character is the game's own.
// Entry redirect on the writer, placed by Tick on the game thread outside the world tick; the replacement acts only on the
// game thread. Logged: Info at placement; the session's hand line per member handed (with this module's Debug line).

namespace lss::platform {
class GameImage;
}

namespace lss::players {
class Session;
}

namespace lss::players::event_move_hand {

/// Checks the writer against the research (the name lookup is world/named_actor's); the redirect is placed by Tick. False
/// (logged) when a check fails: the session then hands a member after the write (a one-shot animation on it can end at once).
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The session the hands go through. Set once after the install; game thread.
void SetSession(Session* session) noexcept;

/// Game thread, each engine tick outside the world tick: places the redirect on the first call.
void Tick() noexcept;

}  // namespace lss::players::event_move_hand
