#pragma once

// Where the HUD's main roots sit in a split half: as in single player, each in its corner of the half
// (research/RE-hud-band-3d.md): minimap top right, compass top centre, command menu bottom
// left, party panel bottom right, and the two summon gauges (the "SUMMON" bar and the summoned creature's name and
// time) with the party panel, where their design puts them above it.
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
//
// The HUD size (the settings file's [Views] hud-scale for player 1, hud-scale.player-2 and .player-3 for the others, each
// in their own view) scales these roots alone, each about the corner it is anchored to (SetRenderTransformPivot at that
// corner, SetRenderScale), so each stays in its corner of the view: in a split window's band, never on a row of monitors
// or in a whole-screen moment. Everything else on the overlay (enemy names, damage numbers, lock-on marks, prompts over
// objects) keeps the game's size. The roots are one set painted again in each view's pass (hud/second_image.h), and each
// pass reads a root's render transform at its own paint: the tick holds them (the corner's pivot, player 1's scale)
// while any player's size is not the game's, and each pass gives them its own player's scale just before it paints
// (SizeForPass). The game's own scale and pivot are read first and set back when no size is held and when the split
// ends; a root is left at the game's transform while the game scales it. Game thread only, but for the settings'
// console commands, which only store the sizes.

#include "hud/overlay_band.h"

namespace lss::hud::root_placement {

/// Game thread, every tick, before the frame's render: whether the band lays the HUD out in a split half. Places the
/// listed roots (a new instance as soon as it is listed) while it does, and sets every placed root back when it stops.
void Tick(bool bandApplies) noexcept;

/// Sets every placed root back (unload).
void Remove() noexcept;

/// Reads the players' HUD sizes from the settings (a file with only player 1's gives it to players 2 and 3 once) and
/// registers their console options.
void Install() noexcept;

/// The corner of the frame the summon gauge (the widget whose tick places the glow) is anchored to in a split half: the
/// UI particles' boxes follow it (hud/overlay_band.h EffectViewport).
LayoutPoint GlowAnchor() noexcept;

/// The HUD size that widget was drawn at in `column`'s pass this frame (1 while it is not placed or not held): the
/// particles' boxes scale with it.
float GlowSizeOf(int column) noexcept;

/// Game thread, in the overlay render: just before `column`'s pass paints, the held roots take the size of `seat`'s
/// player (the view's own; player 1's for 0 and for a mirror's), each set through the game's setter only when it differs
/// and only while the root still shows the scale the mod set last (a root the game set during the frame is left alone
/// and let go at the next tick). The game's own pass, the frame's last, is given player 1's, so every tick sees it.
void SizeForPass(int column, int seat) noexcept;

}  // namespace lss::hud::root_placement
