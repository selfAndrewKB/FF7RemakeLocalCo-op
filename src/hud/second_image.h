#pragma once

// Player 2's HUD image: the game's own overlay rendered a second time each frame, first, into a second layer entry of
// the same pool-2 ring slot (RE-hud-overlay-pass.md §10; Rebirth's hud/second_image, the same mechanism re-found).
//
// The overlay's render delegate (vtable slot 0x144BF4DB0, the thunk 0x14151BAF0 that calls
// SEndRootOverlay__RenderToInGameUILayer with the overlay) is replaced by a driver that, while player 2's image is on
// and the split is shown, runs:
//   1. player 2's pass: the game's render, with DeltaTime 0 (g_FApp_DeltaTime, read by the paint's widget ticks) and
//      the overlay's fade saved and restored (it advances by the world's delta instead), so no timer or animation
//      advances twice. Its slot acquire is the real one and is remembered (the acquire call 0x141509FCF, crossing a
//      block: installed from the game thread outside the render); its layer entry is named for player 2 (hud/
//      overlay_layout's entry call), laid out in player 2's view; its submit is skipped; the navi map's pool 0 is
//      handed over between the passes (hud/minimap_two.h);
//   2. player 1's pass: the game's own call. Its acquire returns the remembered slot, its entry is the stock one, and
//      its submit sends the slot holding [player 2's entry, player 1's entry].
// Player 1 renders last, so everything the overlay keeps from its last pass (its rects, hit-test grid, window size,
// fade) is player 1's, as without player 2. The render thread draws each entry in its own view (hud/image_entries.h)
// into the view's own target (hud/ui_targets.h, switched on with the image).
// Content: player 2's image shows what the HUD holds this frame (player 1's) until player 2's content follows.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::second_image {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Both players' ULocalPlayer and player controller, for the marker replays.
struct Players
{
    void* localPlayer[2] = {};
    void* controller[2]  = {};
};

/// Game thread, every tick, before the frame's render: installs the acquire call once and records whether the split is
/// shown and who the players are.
void Tick(bool splitShown, const Players& players) noexcept;

/// The `hud-image` switch (on from a successful install).
void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

/// The FName value of player 2's layer entry for the pass running now (the stock one otherwise). Game thread.
unsigned long long EntryNameForPass(unsigned long long stockName) noexcept;

}  // namespace lss::hud::second_image
