#pragma once

// Lays Square's HUD out inside the part of its UI image a split view shows, so each half shows the whole HUD,
// anchored as a native screen of the half's shape, with its world markers on their objects (hud/overlay_band.h has
// the arithmetic; RE-hud-split.md §1, RE-hud-overlay-pass.md §3, §5).
//
// Two calls of the overlay's own per-frame render (SEndRootOverlay__RenderToInGameUILayer 0x141509D00, game thread)
// are redirected (platform/call_redirect.h); both pass the game's values through unless the split is shown and the
// band is on:
//   - EndUI__GetInGameUIRect (0x141509E9D) returns the band the view's cover fit shows instead of the whole image,
//     so the widget tree lays itself out at the band's size and anchors (a native 32:9 screen for a top/bottom half).
//     The call crosses a 16-byte block, so it is installed from the game thread, outside the render (Tick);
//   - FEndMenuRenderer__FindOrAddNamedLayerEntry (0x14150A006): after it, the entry's matrix gets the terms that draw
//     that layout into the band unstretched (the game's matrix ignores the rect's Min), and the overlay's marker host
//     rect becomes the pass's player's view: origin and size in window pixels, and the host scale that keeps markers on
//     their objects. Square's own OnPaint fallback sets the host rect to the first player's view rect; the delegate
//     path uses the whole window only because single player's view is the window (RE-hud-split.md §1.4). The HUD
//     ticks, which run before the next render, read these values.
// Each pass lays out for its own player's view: player 1's, or player 2's in player 2's pass (hud/second_image.h),
// whose entry gets player 2's name, and whose submit is left to player 1's pass.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::overlay_layout {

/// A local player's view as fractions of the window (ULocalPlayer Origin and Size).
struct ViewShare
{
    float originX = 0.0f;
    float originY = 0.0f;
    float sizeX   = 1.0f;
    float sizeY   = 1.0f;
};

/// Checks the two call sites and the contain-scale leaf, installs the entry redirect and arms the rect redirect
/// (installed by the next Tick). False (logged) if anything differs: the HUD keeps the game's layout.
bool Install(const platform::GameImage& image) noexcept;
/// Puts the calls back (the rect call only from the game thread). At unload.
void Remove() noexcept;

/// Game thread, every engine tick before the frame's render: installs the rect call once, and records whether the
/// split is shown and each player's view (player 2's for player 2's pass, hud/second_image.h).
void Tick(bool splitShown, const ViewShare& playerOne, const ViewShare& playerTwo) noexcept;

/// Writes player `player`'s (1 or 2) marker host set, as that player's pass last laid it out, into the overlay
/// (+0x388..+0x3A8), for marker placements made outside the render (hud/marker_replay.h); the render writes the same
/// values again in that player's pass. False before that pass has run with the band. Game thread.
bool WriteHostSet(int player) noexcept;

/// Whether the last overlay render laid the HUD out in a band. Game thread.
bool BandApplies() noexcept;

/// The `hud-band` switch (on at start).
void SetBandEnabled(bool on) noexcept;
bool BandEnabled() noexcept;

}  // namespace lss::hud::overlay_layout
