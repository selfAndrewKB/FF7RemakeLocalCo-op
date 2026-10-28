#pragma once

// Every further player leaves before the game leaves the field (Remake/Documentation/research/RE-lifecycle-world.md §3, §8).
//
// A retry, a save load, a chapter or story jump and the return to the title all leave the field inside the same
// world, with no map load: the game state enters "leaving" (9), whose entry calls the field root's exit request.
// The request clears the running byte, and from the next frame the step machine tears the field down and rebuilds
// the party. The request has two callers, the state entry's call and the Blueprint ExitLoation thunk's tail jump.
// Both are redirected (platform/call_redirect.h), so `leave` runs inside the request, before the game's own work,
// while the party is still intact. players::Session keeps a check of the field step as the fallback.
//
// Rebirth (player_two/field_exit) redirected the request's entry. Remake's entry starts with a 2-byte push, too
// short for a jump, so the two call sites are redirected instead. The step and running byte are read by
// players/game_state, not by a reader of this module's own.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::players::field_exit {

/// Called inside the request while the field still runs; `caller` names which of the two callers asked.
using LeaveFn = void (*)(void* context, const wchar_t* caller);

/// False (logged) if a call site is not the call the research read; then only the fallback catches an exit.
bool Install(const platform::GameImage& image, LeaveFn leave, void* context) noexcept;

/// Puts both calls back (each one still ours); the request then runs as the game's alone.
void Remove() noexcept;

}  // namespace lss::players::field_exit
