#pragma once

// Each seat's own minimap and compass: a navi map manager per seat, the game's own (research/RE-hud-minimap-two.md;
// the pool-0 handover between the frame's passes is written in hud/minimap_rules.h).
//
// EndUISystem[29] is one manager (0x268 bytes) owning the minimap root NaviMap_C, its octagon map, icons, footprints and
// the compass, all centred on the party leader and turned by the primary camera. Each seat gets an instance of its own,
// made with the game's allocator and constructor and bound to the banner fades as the game binds its own:
//   - its tick runs right after the game's (its call in the HUD tick), with the real DeltaTime, as the seat:
//     inside the seat's primary and leader contexts (players/scopes.h: the primary controller PM+0 the camera reads, the
//     leader type byte PM+0x233 the centre reads, and the leader's weak pair, which the Leader context lends too; the
//     tick's reads to depth 3 are the two former, RE-hud-minimap-two.md, deeper ones [UNKNOWN]), with the compass's
//     first player controller (one call in the tick) the seat's, the slot EndUISystem[29] the seat's instance (the
//     compass reads the global slot's icons), and the seat's column's marker host set; the tick is skipped, with a
//     line, while the contexts cannot be lent (a member not bound);
//   - its octagon renders into a texture of its own: while a seat's root is created (the creator's AddToRootOverlay
//     call) the octagon's RenderTexture is nulled, so the game's Construct makes a private target;
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

/// The game's icon parameter block (AddIcon's sixth argument, RE-lead-marker.md).
struct IconParams
{
    float         lineLength         = 0.0f;  // the diamond's line height (cm; 150 when about 0)
    float         viewTargetDistance = 0.0f;  // the minimap's "new" flag clears within it (2000 when 0)
    float         circleRadius       = 0.0f;  // > 0: an area icon, no diamond
    float         visibleStart       = 0.0f;  // the diamond shows within it of the view's character (else the widget's range)
    std::uint8_t  hidden             = 0;     // not on this instance's minimap, compass or area map (the diamond ignores it)
    std::uint8_t  onCanvas           = 0;     // on the instance's icon canvas whatever its kind
    std::uint8_t  pad[2]             = {};
    std::int32_t  layer              = -1;    // the floor; -1: the map's bounds
};
static_assert(sizeof(IconParams) == 24, "the game's block");

/// Game thread, outside the HUD tick: the game's AddIcon on one instance only, not repeated on the others: the game's
/// (`seat` 0, EndUISystem[29]: the one the field diamond reads) or seat `seat`'s own. A key already there is updated in
/// place. False when that instance is not there (not made yet, faulted) or the call faulted.
bool AddIconTo(int seat, std::uint64_t key, std::uint8_t kind, const float (&at)[3], const IconParams& params) noexcept;
/// The game's RemoveIcon on one instance only (the widget and the entry go at once; a missing key is a no-op:
/// RE-lead-mark-check.md).
bool RemoveIconFrom(int seat, std::uint64_t key) noexcept;
/// Game thread: the icon `key` on seat `seat`'s own instance is drawn `scale` times the game's per-icon scale, through
/// the game's setter after each of the seat's ticks (the tick writes the game's scale every frame). Key 0 or scale 1.0
/// stops it. One key per seat (its own marker); player 1's icons are untouched.
void SetPinScale(int seat, std::uint64_t key, float scale) noexcept;
/// The field ends and resets heard: each empties every instance's icons, so an icon added before is added again.
std::uint32_t IconResets() noexcept;

}  // namespace lss::hud::minimap
