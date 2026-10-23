#pragma once

// A test for the blue haze in players 2's and 3's views (RE-haze-history.md, S-RMK-HAZEHIST): the game's own camera cut
// on every camera but player 1's, held for a number of frames.
//
// The haze comes only when players 2 and 3 join within about a second of a load, and it stays with their columns
// through a leave and into the mirror. A camera cut makes the renderer drop each view's frame-to-frame history (TAA,
// the scene-colour copy, depth of field, volumetric fog, distortion, Square Enix's AO): each is replaced by a dummy
// texture or a no-history shader, never scaled by a weight [CONFIRMED code]. The cut is the stock
// APlayerCameraManager::SetGameCameraCutThisFrame (PCM +0x1E18 bit 0x400); the engine clears it on every manager after
// each draw, so a held cut is raised again after each update. What the test tells:
//   - the haze goes and stays gone: a history that fed itself (fix: one cut at each join);
//   - it goes during the cut and comes back at once: the history textures' contents (the pool hands the same ones
//     back; the read position is then the suspect, RE-haze-history.md candidate 1);
//   - no change: not a history (the mod's per-column fixes or the light shafts are next).
// Command `history-cut <frames>` (0 stops); reading `history-cut.raised` (cuts raised since the start).

namespace lss::platform {
class GameImage;
}

namespace lss::render::history_cut {

/// Verifies the game's cut setter. False (logged) when it differs; the test is then unavailable.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Right after a camera manager's update (game thread). Player 1's (`playerOne`): one frame of the hold is counted.
/// Any other (a seat's or a mirror player's): the cut is raised on it while the hold lasts.
void AfterCameraUpdate(void* manager, bool playerOne) noexcept;

}  // namespace lss::render::history_cut
