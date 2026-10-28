#pragma once

// Each seat's own minimap and compass: a navi map manager per seat, the game's own (RE-hud-minimap-two.md; Rebirth's
// area_icons_two and minimap, the pool-0 handover taken from them, written for the frame's passes in
// hud/minimap_rules.h; the seats follow plan/seats.md step 3).
//
// EndUISystem[29] is one manager (0x268 bytes) owning the minimap root NaviMap_C, its octagon map, icons, footprints and
// the compass, all centred on the party leader and turned by the primary camera. Each seat gets an instance of its own,
// made with the game's allocator and constructor and bound to the banner fades as the game binds its own:
//   - its tick runs right after the game's (the call 0x1413AE3A0 in the HUD tick), with the real DeltaTime, as the seat:
//     inside the seat's primary and leader contexts (players/scopes.h: the primary controller PM+0 the camera reads, the
//     leader type byte PM+0x233 the centre reads, and the leader's weak pair, which the Leader context lends too; the
//     tick's reads to depth 3 are the two former, RE-hud-minimap-two.md, deeper ones [UNKNOWN]), with the compass's
//     first player controller (the call 0x1413EFDE9) the seat's, the slot EndUISystem[29] the seat's instance (the
//     compass reads the global slot's icons), and the seat's column's marker host set; the tick is skipped, with a
//     line, while the contexts cannot be lent (a member not bound);
//   - its octagon renders into a texture of its own: while a seat's root is created (the creator's AddToRootOverlay
//     call 0x141473F36) the octagon's RenderTexture is nulled, so the game's Construct makes a private target;
//   - the game's events that only reach EndUISystem[29] (map set-up, field start and end, reset, icon add and remove:
//     call redirects) are made on every seat's instance too, and each instance's shown/enabled/mode flags follow
//     player 1's through the game's own setters;
//   - per pass (hud/overlay_pass.h: a column's pass paints that column's map; a seat's column is its index): every
//     other player's roots are transparent (SetColorAndOpacity, restored after the pass; the manager writes the root
//     alpha from its fade every tick, so nothing animated is lost), and one other octagon does not render its map
//     (its pool-0 span skipped, hud/image_code.h: player 1's in a further pass, the first seat's in column 0's; a
//     third octagon in a pass not its own renders to its own target through the game's acquire, its span skipped
//     too: image_code::kNaviSkips holds one skip per view);
//   - pool 0 turns once a frame (minimap_rules::PoolHandover): the first further pass's octagon acquires and holds
//     the slot, every later pass's octagon is handed it and names its entry with its column's number, column 0's
//     submit sends them all; a slot still held after the frame's last render is sent then.
// A seat's instance is made once and kept for the process (the game keeps its own); when the seat's player leaves it
// is hidden with the game's setter and its map pieces released. Game thread only.

#include "hud/overlay_pass.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::minimap {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;
/// The navi map's pool-0 code is installed (the further columns' images need it).
bool PoolCodeInstalled() noexcept;

/// Game thread, every tick, outside the HUD, before the seats' ticks: places the one event redirect that crosses a
/// block, from the game thread.
void Tick() noexcept;

/// Game thread, every tick, per built seat: whether the seat's player plays a member in the split, and the seat's
/// controller. Makes, mirrors, hides and releases the seat's instance.
void TickSeat(int seat, bool seatPlays, void* seatController) noexcept;

/// Game thread, from the overlay driver: before each pass (the column whose pass runs, overlay_pass::kSingle for the
/// game's one render) and after it, and after the frame's last render.
void BeginPass(int column) noexcept;
void EndPass() noexcept;
void EndFrame() noexcept;

/// The `hud-minimap` switch (on at start).
void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

}  // namespace lss::hud::minimap
