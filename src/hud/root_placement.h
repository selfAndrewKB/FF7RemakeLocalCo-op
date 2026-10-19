#pragma once

// Where the HUD's main roots sit in a split half: as in single player, each in its corner of the half (RE-hud-band-3d.md
// Q5; Rebirth's hud/hud_placement, the user's reference there: minimap top right, command menu bottom left, party panel
// bottom right).
//
// The roots' Blueprints design their contents for a 1920x1080 frame inside a widget that fills the layout. In single
// player the layout is that frame, so the contents land in its corners; in a split half's 3840x1080 (or 1920x2160)
// band layout they stay in a centred 1920x1080 box (the minimap at the box's top right, not the half's).
//
// Every root on the HUD overlay is arranged by its viewport slot: anchors, offsets and alignment (UUserWidget+0x1B0,
// +0x1C0, +0x1D0, read each frame by the SEndCanvas wrapper UEndUserWidget__AddToRootOverlay made). While the band
// applies, each listed root gets, through its own reflected setters (SetPositionInViewport, SetDesiredSizeInViewport,
// SetAlignmentInViewport, SetAnchorsInViewport), a 1920x1080 area at its corner of the band. In a 1920x1080 layout
// that area is the whole layout, so nothing changes in single player. The game's own layout is read first and set
// back through the same setters when the split ends; a root whose layout the game set since is left with the game's.
// Game thread only.

namespace lss::hud::root_placement {

/// Game thread, every tick, before the frame's render: whether the band lays the HUD out in a split half. Places the
/// listed roots (a new instance as soon as it is listed) while it does, and sets every placed root back when it stops.
void Tick(bool bandApplies) noexcept;

/// Sets every placed root back (unload).
void Remove() noexcept;

}  // namespace lss::hud::root_placement
