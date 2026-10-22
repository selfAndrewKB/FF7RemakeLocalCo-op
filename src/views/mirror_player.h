#pragma once

// The mirror player: an extra local player of the mod's in every column no player fills, showing player 1's picture,
// so a spanned window never shows one view across the whole row (plan/topics/views.md; the user's policy of
// 2026-10-22: the mirror, always, for everything outside field gameplay).
//
// The engine's own levers. UGameplayStatics::CreatePlayer for the empty column's controller id (as player 2's join,
// through players/local_player_list inside the camera-manager join scope of players/own_controller), so the engine
// lays the columns out and renders the mirror's view as a player's. Its End camera manager shows player 1's camera
// because every pose slot of player 1's manager, the field camera's slot 24 included, is set on it with the game's
// own slot setters right before its update (views/scene_camera_two, which does the same for the scene slots on
// player 2's manager): its blends switched on as player 1's are, player 1's camera cut raised on it, the cutscene
// end's reset run on it. The mirror has no pad, no character and no field camera operator: the game ticks the primary
// controller's input only, so its controller is inert.
// Player 2's join takes the mirror's column: the session takes the mirrors out inside its join, right before its
// CreatePlayer, and the mirror comes back on the tick player 2 leaves (its Tick runs after the session's), so no
// tick draws with the column empty. The mirror stays through a field exit: a retry, a save load, a jump and the
// return to the title all keep the world and the local players (players/field_exit.h), so a loading screen draws in
// its column too [first live check: v0.42.0]. It is added only in a steady field (the title is one), and again if
// the engine drops it.
// Rebirth's views/third_view (read 2026-10-22) gave the shape: a local player of the mod's beside the session's,
// removed before player 2 leaves. Changed: Remake's End manager ignores every view target but its own camera actor
// (research/RE-camera.md §2.1), so the camera is a slot copy, not a SetViewTarget re-aim; the mirror fills the
// column player 2 is out of instead of needing player 2 in; it stays through world changes.
//
// Wanted while the span is held (views/window_span) with fewer players than columns. `mirror-player on|off` (on),
// reading `mirror-player.state`. Game thread only. Logged at Info on each add and remove, at Debug for the status.

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::views::mirror_player {

/// Finds RemoveLocalPlayer and registers the commands. False (logged) if the function differs from the research.
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick after the session's and the span's: adds the mirror for an empty column once the field is
/// steady, removes it when its column is a player's, the span is not held or the switch is off. `playersIn` counts
/// the real players (1 or 2); `columns` the span's columns (0 when the span is out or not held).
void Tick(std::int32_t playersIn, std::int32_t columns) noexcept;

/// Takes every mirror out (`why` logged): the session's join, the switch, the unload.
void Clear(const wchar_t* why) noexcept;

/// The mirror into player 2's column on the tick player 2 leaves for a world change (the session calls it right
/// after the leave), so the loading screen that follows is drawn by two views, not one stretched across the row.
/// The field is leaving, not steady; the other checks hold. Logged either way.
void FillAtLeave(const wchar_t* why) noexcept;

/// The mirrors in now (0 or 1 today).
std::int32_t Count() noexcept;

/// The `mirror-player.state` reading.
std::wstring Reading() noexcept;

}  // namespace lss::views::mirror_player
