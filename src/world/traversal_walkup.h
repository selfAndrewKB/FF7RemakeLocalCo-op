#pragma once

// Players 2 and 3 walk their member up to ladders, squeezes, crawls, monkey bars, climbs, jumps, vaults, rubble,
// duck-unders, slides, fence gaps, steps, balance beams, fans, counter gates and lifting ropes, as player 1 does
// (the user's report of 2026-10-21, picked 2026-10-24; research/RE-traversal-walkup-two.md, RE-traversal-items.md
// §10-§13; the action itself: world/traversal_action.h).
//
// Every walk-up spot is a field-action actor. In single player the primary controller's PlayerTrigger enters the spot's
// volume, the actor's enter stores the party leader as its one user, and every frame the actor's TryStart tests that
// user (moving toward an end, the stick turned by the primary camera) and requests the action on the user's own
// field-action component. Nothing of that reaches a further player: the game sweeps only the primary controller's
// trigger, and the user is always the leader. Rebirth's trigger_two did the same job (cleanup-2026-10; taken: a
// second pass right after the game's, admitted classes only, the rest held back and logged; changed: Remake's
// dispatcher resolves the trigger through the party manager, so the pass is the game's own dispatch with the seat's
// controller answered at that one call, and the per-player state is the actor's user block):
//   - the sweep: after the game's sweep of player 1's trigger, the game's sweep runs for each seat's own trigger copy
//     (players/player_trigger.h), which adds the seat's entry to the same pending list; a queue records whose each
//     PlayerTrigger entry is, and the dispatcher's resolve of that entry answers the seat's controller;
//   - the filter: a seat's enter and leave go to the game's router only for the admitted walk-up classes, inside the seat's
//     Leader scope (the enter stores the seat's member) with the seat's own copy of the actor's user block swapped in
//     (so the players never overwrite each other's); everything else a seat's trigger touches (story and encounter
//     triggers, boxes, doors with their own enter) is held back. The volumes the dispatcher lists go to the seat's own
//     controller's list, and player 1's minimap area calls are skipped for a seat;
//   - the start: each class's TryStart runs again for a seat whose copy names its member, inside the seat's Leader
//     and Primary scopes with pad 2's or 3's stick: the game's own test then reads the member, the seat's stick and
//     the seat's camera, and requests on the member's component. Around that run the member branch's ExceptionFlg
//     (else it would request player 1's leader), player 1's camera-cut stick latch, the crawl's cooldown and player
//     1's "an action is running" switch (the user, 2026-10-24: a crawl locks only the mover) are the seat's;
//   - the arrows: each class's markers update gets the nearest player's member instead of player 1's leader (the user,
//     2026-10-24: arrows near either player).
// A spot whose ignore group lists the member is skipped, as the game skips it for its leader. Game thread only (the
// dispatcher, the sweep and the TryStart loop all run there). Logged: Info at install, Debug per seat enter, leave,
// hold-back and start request, Trace per re-run.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::traversal_walkup {

/// Checks every site and slot against the research and places the redirects (logged). False if a check failed:
/// further players then cannot use walk-up spots.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick: the seat's controller and member while its player is in (null otherwise). A new
/// controller or trigger drops the seat's copies (a new controller also its queued entries, at their resolve).
void TickSeat(int seat, void* controller, void* member) noexcept;

void Remove() noexcept;

/// The `traversal.walkup` reading: per seat the spots it is in, the deliveries, hold-backs and start requests.
std::wstring Reading() noexcept;

}  // namespace lss::world::traversal_walkup
