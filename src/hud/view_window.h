#pragma once

// The game's own framed message window in one player's view: the claim screen's prompts (players/pad_claim;
// research/RE-game-widgets.md §12).
//
// Each window is the game's Com_Window_01_C (UEndMessageWindow), made and shown by the game's own sequence
// (EndUI_ShowMessageWindow's: create, add to the HUD overlay, text, no options, open with its In animation and sound,
// the game's window list) at a ZOrder above the HUD and below subtitles and the pause menu. Its text is the game's
// markup (button icons, colours), drawn by the game's text block in the game's font. Open always pushes a focus layer
// (no game path shows one without); the window's own layer is popped in the same call (UEndUserWidget::
// PopFromFocusStack), so focus never leaves what had it and no key ever reaches the window: the claim reads the pads
// itself. Every root on the overlay is painted in every player's column; hud/pass_hide makes a window transparent in
// every pass but its view's (ViewOf), as it does for the cursors, and the window's animations never write that
// colour (§12.5). A closed window plays the game's Out and close sound and is taken out of the overlay by its own tick
// (the game's remove-after-Out byte). A load or map change closes the windows through the game's list. Opens and
// closes logged at Info, text changes at Debug. Reading `views.window`. Game thread.

#include "hud/overlay_pass.h"

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::view_window {

/// A window's view: a seat's (0 player 1's, the further seats'), or a copy of player 1's picture in a column of its own
/// (MirrorView), shown in that copy's own HUD pass (hud/second_image) only.
inline constexpr int kSeatViews = 4;  // players::scopes::kMaxPlayers
constexpr int MirrorView(int column) noexcept
{
    return kSeatViews + column;
}
/// Every view a window can be keyed by.
inline constexpr int kViews = kSeatViews + overlay_pass::kMaxColumns;

bool Install(const platform::GameImage& image) noexcept;
/// Forgets the windows without a UI call (a window left open stays on screen until the game's next reset).
void Remove() noexcept;

/// Shows `markup` in `view` (a seat's, 0 player 1's, or MirrorView(column)): opens its window, or changes the open
/// one's text. False when no window can be shown (not installed, no world, the game made none, or called inside a
/// player's scope, where the game's focus context is that player's copy): the caller has its own fallback.
bool Show(int view, const std::wstring& markup) noexcept;

/// Closes `view`'s window (Out and the close sound), if one is open.
void Close(int view) noexcept;
void CloseAll() noexcept;

/// The view whose window, open or closing, `widget` is, or -1. For hud/pass_hide, during the HUD passes.
int ViewOf(const void* widget) noexcept;
/// Some window is open or still closing.
bool Any() noexcept;
/// How many open windows the game has closed (a load, a map change, a script's clear): a rise tells the caller to show
/// its texts again.
unsigned long long ClosedByGame() noexcept;

/// The game's own menu sounds, for a step in a window's player's choice.
enum class Sound : unsigned char
{
    Cursor,  // UI000_Cursor: a move
    Decide,  // UI002_OK
    Cancel,  // UI003_Cancel
};
void Play(Sound sound) noexcept;

/// Once per engine tick on the game thread: forgets windows the game closed or took out of the overlay.
void Tick() noexcept;

}  // namespace lss::hud::view_window
