#pragma once

// Player 2's own minimap and compass: a second navi map manager, the game's own (RE-hud-minimap-two.md; Rebirth's
// area_icons_two and minimap, the pool-0 handover taken from them).
//
// EndUISystem[29] is one manager (0x268 bytes) owning the minimap root NaviMap_C, its octagon map, icons, footprints and
// the compass, all centred on the party leader and turned by the primary camera. Player 2 gets a second instance, made
// with the game's allocator and constructor and bound to the banner fades as the game binds its own:
//   - its tick runs right after the game's (the call 0x1413AE3A0 in the HUD tick), with the real DeltaTime, under player
//     2's scopes: the leader type byte PM+0x233, the primary controller PM+0, the compass's first player controller (the
//     call 0x1413EFDE9), the slot EndUISystem[29] itself (the compass reads the global slot's icons), and player 2's
//     marker host set;
//   - its octagon renders into a texture of its own: while player 2's root is created (the creator's AddToRootOverlay
//     call 0x141473F36) the octagon's RenderTexture is nulled, so the game's Construct makes a private target;
//   - the game's events that only reach EndUISystem[29] (map set-up, field start and end, reset, icon add and remove:
//     call redirects) are made on player 2's instance too, and its shown/enabled/mode flags follow player 1's through the
//     game's own setters;
//   - per pass: the other player's roots are transparent (SetColorAndOpacity, restored after the pass; the manager writes
//     the root alpha from its fade every tick, so nothing animated is lost) and the other player's octagon does not
//     render its map (its pool-0 span skipped, hud/image_code.h);
//   - pool 0 turns once a frame: player 2's octagon acquires and holds the slot in player 2's pass, names its entry with
//     number 2 and skips its submit; player 1's octagon takes the held slot, adds the stock entry and submits both. A
//     slot still held after player 1's render is submitted then (an acquire landing on an unsubmitted slot would spin).
// Player 2's instance is made once and kept for the process (the game keeps its own); when player 2 leaves it is hidden
// with the game's setter and its map pieces released. Game thread only.

#include "hud/overlay_pass.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::minimap_two {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;
/// The navi map's pool-0 code is installed (player 2's image needs it).
bool PoolCodeInstalled() noexcept;

/// Game thread, every tick, outside the HUD: whether player 2 plays in the split, and player 2's controller. Makes,
/// mirrors, hides and releases player 2's instance.
void Tick(bool playerTwoPlays, void* playerTwoController) noexcept;

/// Game thread, from the overlay driver: before each pass and after it, and after the frame's last render.
void BeginPass(overlay_pass::Pass pass) noexcept;
void EndPass() noexcept;
void EndFrame() noexcept;

/// The `hud-minimap-two` switch (on at start).
void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

}  // namespace lss::hud::minimap_two
