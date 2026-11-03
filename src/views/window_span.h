#pragma once

// The game window spanned across a row of monitors, one full 16:9 view per player, through the game's own levers.
//
// One view per monitor needs three things: the engine's side-by-side layout for two players (the stock option
// byte of UGameMapsSettings; the top-and-bottom default otherwise), a render image as wide as the row, and the
// window placed over the row. Square Enix sizes the render image from the game's resolution setting, not from the
// window (research/RE-multi-monitor-views.md §5), so the entry is, in this order:
//   1. the game's own resolution request (FSystemResolution::RequestResolutionChange, the options-menu path) for
//      the row's size in the Borderless mode; the engine applies it on its next frame, and in that mode fits the
//      window to its current monitor (§6.1): the image grows, the window stays;
//   2. then the engine's own window reshape (FWindowsWindow::ReshapeWindow on UGameEngine::GameViewportWindow's
//      native window, the call at the bottom of every path the engine moves its window by) to the row; the engine
//      follows its window (WM_SIZE -> Slate -> the viewport takes the draw size at its next draw, §5).
// Any later resolution request (the options menu, Alt+Enter) fits the window back to one monitor, so while in, the
// rect and the resolution are checked twice a second and both steps are re-applied on a mismatch, logged with what
// was seen. The leave puts the window back where it was, then requests the saved resolution and mode (for a Windowed
// player the engine restores the title bar and its saved placement, then centres; a last reshape puts the client
// area exactly back). Nothing is written to disk: the player's own resolution lives in the device save, which a
// request never touches (§8). Exclusive fullscreen is refused. The Borderless letterbox patch (views/letterbox_patch)
// is a precondition: without it each column is fitted into the middle of the target (§6.3).
// Taken from Rebirth's views/window_span: the chain walk and its checks, the hold, the saved-rect leave. Changed:
// the resolution request in the entry and the leave (the Square Enix image), the fixed order, the layout byte.
//
// Entered on a further player's join when a row of equal monitors qualifies (`window-span on`, the default), or by the
// command / Ctrl+F2; left by the command, with the last further player, and at the mod's end. The number of columns is the number
// of monitors in the row, up to three (plan/seats.md step 5; a three-monitor row is unplayed: none on this PC); the
// mirror player fills the columns no player has. The hold (`window-span.hold on`, v0.41.0) spans the row with any player count and
// keeps it when the last further player leaves (a loading screen, the main menu, a movie): the multi-monitor presentation modes
// (plan/topics/views.md); the mirror player (views/mirror_player, v0.42.0) fills the empty column under it. The
// preset `[Views] multi-monitor = off | on | auto` in LocalSplitScreen.ini (v0.43.0; platform/settings) sets the
// start: `on` holds from the first steady frame of the game (the title), `off` leaves the span to the command,
// `auto` (the default) is the join rule. A row monitor a few pixels off the row's top is lined up first, for the Windows
// session, and put back at the leave (views/display_align; v0.38.0); no display mode or scaling is ever changed: a
// row that does not qualify is logged with the reason and what to change. Game thread only. Logged at Info on each
// step and change, at Debug for the status.

#include <cstdint>
#include <span>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::views::window_span {

/// Verifies the window chain's code, the resolution request and the resolution global in the running build.
/// False (logged) if they are not as researched; the span is then unavailable.
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

/// Spans the game window over `columns` monitors of a row (the entry's first step; the rest follows in Tick).
/// False (logged) if it cannot.
bool Enter(std::int32_t columns) noexcept;

/// Puts the window and the resolution back. `why` is logged. Nothing happens if not in.
void Leave(const wchar_t* why) noexcept;

/// Ctrl+F2: leaves (and stays out) when in, else enters (and enters on each join again).
void Toggle() noexcept;

/// Each engine tick: the entry's steps, the hold, the auto-enter on a further player's join, the leave with the
/// last further player.
void Tick(bool furtherPlayerIn) noexcept;

/// The span is in (or entering).
bool In() noexcept;

/// The hold is on: the span stays with any player count (`window-span.hold`).
bool Held() noexcept;

/// The columns the span is in with (0 when out): one per real player, the rest the mirror player's.
std::int32_t Columns() noexcept;

/// The span's monitors' widths, left to right, one per column (empty when out): the column layout gives each view its
/// own monitor's width (views/column_layout). Game thread.
std::span<const std::int32_t> ColumnWidths() noexcept;

/// The preset (`[Views] multi-monitor`, platform/settings.h; the console's `multi-monitor`), one of its words:
/// `auto` the span while a further player is in, `on` the span held with any player count (from the game's next
/// steady frame when out), `off` neither until `window-span on`, `window-span.hold on` or Ctrl+F2. Applied at
/// install from the file, and at any time from the console: `off` leaves now, `auto` leaves when no further player
/// is in, `on` keeps a span that is in. `why` names the source in the log line. The reading answers the live state
/// (`on` while held, `off` while neither held nor on the join rule, else `auto`), so Ctrl+F2 and the window-span
/// switches show in it.
void SetPreset(const std::wstring& preset, const wchar_t* why) noexcept;

/// The `window-span.state` reading.
std::wstring Reading() noexcept;

}  // namespace lss::views::window_span
