#pragma once

// The mirror players: an extra local player of the mod's in every column no player fills, showing player 1's picture,
// so a spanned window never shows one view across the whole row (plan/topics/views.md; the user's policy of
// 2026-10-22: the mirror, always, for everything outside field gameplay; plan/seats.md step 5: up to two mirrors,
// one per empty column of three).
//
// The engine's own levers. UGameplayStatics::CreatePlayer for each empty column (as a further player's join, through
// players/local_player_list inside the camera-manager join scope of players/own_controller), so the engine lays the
// columns out and renders each mirror's view as a player's. A mirror's column is its place in the engine's local
// player list (the layout goes by that order); its controller id is the lowest id no player in and no other mirror
// holds (the engine refuses a duplicate id, and routes XInput pad N to the local player with id N, so a mirror at
// id 2 swallows pad 3 while it is in: its controller is inert, the game ticks the primary controller's input only).
// Its End camera manager shows player 1's camera because every pose slot of player 1's manager, the field camera's
// slot 24 included, is set on it with the game's own slot setters right before its update (views/scene_camera,
// one copy per mirror, which does the same for the scene slots on a seat's manager): its blends switched on as
// player 1's are, player 1's camera cut raised on it, the cutscene end's reset run on it. A mirror has no pad, no
// character and no field camera operator.
// A further player's join takes a mirror's column: the session takes every mirror out inside its join, right before its
// CreatePlayer, and the mirrors come back on the same tick (their Tick runs after the session's), so no tick draws
// with a column empty. The mirrors stay through a field exit: a retry, a save load, a jump and the return to the
// title all keep the world and the local players (players/field_exit.h), so a loading screen draws in their columns
// too; the column a further player leaves for a world change is filled on that tick (FillAtLeave). A mirror is added only
// in a steady field (the title is one), one per tick, and again if the engine drops it.
// Rebirth's views/third_view (read 2026-10-22) gave the shape: a local player of the mod's beside the session's,
// removed before a further player leaves. Changed: Remake's End manager ignores every view target but its own camera actor
// (research/RE-camera.md §2.1), so the camera is a slot copy, not a SetViewTarget re-aim; the mirrors fill the
// columns no player has instead of needing a further player in; they stay through world changes; two of them.
//
// Wanted while more columns are shown than players are in (views/column_rules.h: the span's columns while held, or
// the console's `view-columns`). `mirror-player on|off` (on), reading `mirror-player.state`. Game thread only.
// Logged at Info on each add and remove, at Debug for the status.

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::views::mirror_player {

/// Finds RemoveLocalPlayer and registers the commands. False (logged) if the function differs from the research.
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick after the session's and the span's: adds a mirror for an empty column once the field is steady
/// (one per tick), removes the mirrors beyond the columns, all of them when the switch is off. `playersIn` counts
/// the real players (player 1 and the seats in); `columns` the columns the frame shows (at least the players);
/// `seatIds` the controller ids the players hold (bit i: id i; bit 0 player 1), so a mirror never takes a seat's;
/// `columnsAlone` the columns with player 1 alone (the span's, or the console's: what a leave for a world change
/// fills; in a plain split that is one, so nothing is filled).
void Tick(std::int32_t playersIn, std::uint32_t seatIds, std::int32_t columns, std::int32_t columnsAlone) noexcept;

/// Inside a seat's leave for a world change (players/session), with that seat already out of the engine's list:
/// fills the empty columns now, before the loading screen, where the steady-field add would come too late.
/// `playersIn` and `seatIds` as the list stands after the leave.
void FillAtLeave(const wchar_t* why, std::int32_t playersIn, std::uint32_t seatIds) noexcept;

/// Removes every mirror now (the session's join, the switch, the mod's end). `why` is logged.
void Clear(const wchar_t* why) noexcept;

/// How many mirrors are in the engine's list.
std::int32_t Count() noexcept;
/// A mirror's view is at that place of the engine's list (as of this tick's Tick or FillAtLeave).
bool InColumn(std::int32_t column) noexcept;

std::wstring Reading();

}  // namespace lss::views::mirror_player
