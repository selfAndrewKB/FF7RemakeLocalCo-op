#pragma once

// A further player's own marker for the move it keeps (players/lead_keep_rules.h; research/RE-lead-marker.md §1, §7):
// the game's own main-objective diamond, placed in the world to guide that player.
//
// While a seat's member runs a move the lead gate judges KEEP (lead_gate::KeptMark), the move's point carries the
// game's own navi icon, kind 2 (the main objective: the diamond with its line, the minimap pin, the compass mark and
// the distance), under the mod's key LSS_LeadMark_<seat>, one the game's keys (story step IDs, MapIconInfo rows,
// <object>_SideDestination) cannot meet:
//   - in the game's instance (EndUISystem[29], the one the field diamond reads), with the hide byte set: player 1's
//     minimap, compass and area map skip it, and hud/field_icons shows its diamond in the seat's column only;
//   - in the seat's own minimap instance (hud/minimap), without it: that player's pin, compass mark and distance.
// Both are the game's AddIcon, made on one instance each (not the mirrored path). Moved when the point changes (the
// level's next move), removed when the verdict ends (the member brought to the point, the setting's end, the player
// letting go) and added again after a field end or reset emptied the icons. Checked every 10 ticks per seat (a row
// changes a few times a minute at most). Without this module the lead gate hands every needed move to its AI.
// Game thread.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::lead_mark {

/// Registers the `hud.lead-mark` reading and tells the lead gate a kept move can be marked.
bool Install(const platform::GameImage& image) noexcept;
/// Removes every seat's marker; the lead gate hands needed moves to the AI again.
void Remove() noexcept;

/// Game thread, every tick, per built seat: the seat's member while its player is in (null otherwise).
void TickSeat(int seat, void* member) noexcept;

}  // namespace lss::hud::lead_mark
