#pragma once

// The further columns' HUD images: the game's own overlay rendered again each frame for each further column
// (hud/overlay_pass.h), before the game's own render, into a layer entry per column of the same pool-2 ring slot
// (RE-hud-overlay-pass.md §10; Rebirth's hud/second_image, the same mechanism re-found; built for one further column,
// keyed by column in plan/seats.md step 4).
//
// The overlay's render delegate (vtable slot 0x144BF4DB0, the thunk 0x14151BAF0 that calls
// SEndRootOverlay__RenderToInGameUILayer with the overlay) is replaced by a driver that, while the image is on and the
// split is shown, runs:
//   1. a pass per further column, the highest first: the game's render, with DeltaTime 0 (g_FApp_DeltaTime, read by the
//      paint's widget ticks) and the overlay's fade saved and restored (it advances by the world's delta instead), so
//      no timer or animation advances twice. The frame's first pass makes the real slot acquire and holds the slot
//      (the acquire call 0x141509FCF, crossing a block: installed from the game thread outside the render); every later
//      pass takes the held slot; each pass's layer entry is named for its column (hud/overlay_layout's entry call) and
//      laid out in its column's view; its submit is skipped; the navi map's pool 0 is handed over between the passes
//      (hud/minimap.h);
//   2. column 0's pass, player 1's: the game's own call. Its acquire consumes the held slot, its entry is the stock one,
//      and its submit sends the slot holding [the further columns' entries, player 1's entry].
// Player 1 renders last, so everything the overlay keeps from its last pass (its rects, hit-test grid, window size,
// fade) is player 1's, as without the split. The render thread draws each entry in its own view (hud/image_entries.h)
// into the view's own target (hud/ui_targets.h, switched on with the image).
// Content: a further column's image shows what the HUD holds this frame (player 1's) except where that column's own
// content follows (the per-pass modules and the replays). A mirror's column with a pass of its own (Columns) shows
// player 1's content throughout (overlay_pass::ShownSeat 0: the hides, the cursor, the command menu and the minimap of
// player 1's pass); only its layout and the placement of player 1's markers, off-screen enemy arrows, search arrows and
// target icons (through the mirror's own view: marker_replay, outside_icons, world/search_arrows PlaceForMirror,
// world/target_icons PlaceForPass) are its own.

#include "hud/overlay_pass.h"

namespace lss::platform {
class GameImage;
}

namespace lss::hud::second_image {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The columns with a pass this tick: player 1's (0) and one per seat in, in the engine's local-player order (a seat's
/// column is its index, players/seats.h), and the mirrors' columns after them when one has another shape than player
/// 1's view (modules.cpp MirrorsOwnImage): a mirror's pass shows player 1's HUD laid out for its own monitor
/// (overlay_pass::ShownSeat 0). The mirrors' columns beyond `count` show column 0's image and get no pass. Per column:
/// its ULocalPlayer and player controller for the replays, and for a seat's column the seat's member (the field icons'
/// range gate).
struct Columns
{
    int   count                                 = 1;
    void* localPlayer[overlay_pass::kMaxColumns] = {};
    void* controller[overlay_pass::kMaxColumns]  = {};
    void* member[overlay_pass::kMaxColumns]      = {};
};

/// Game thread, every tick, before the frame's render: installs the acquire call once and records whether the split is
/// shown (a further column has a pass) and who fills the columns.
void Tick(bool splitShown, const Columns& columns) noexcept;

/// The `hud-image` switch (on from a successful install).
void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

/// The FName value of the layer entry for the pass running now: the stock name, or a further column's own
/// (overlay_pass::EntryName). Game thread.
unsigned long long EntryNameForPass(unsigned long long stockName) noexcept;

}  // namespace lss::hud::second_image
