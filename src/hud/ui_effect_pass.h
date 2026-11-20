#pragma once

// The UI particles (the summon gauge's glow and every other UI effect) placed and sized per column. The game draws them
// per view into the view's UI image, right after the HUD delegate, under the whole image's viewport, and the effect's own
// vertex path puts them in screen space (research/RE-summon-gauge.md). One UI image per column under the mod
// (hud/ui_targets.h), so once per column. The particle itself is one per effect (one position, one size for every
// column: the game converts its layout point once, centred in the layout the overlay holds at that conversion and in
// its half-widths), and a viewport is the one map every vertex goes through: set to a box of that layout's width at the
// column's band's scale and the column's view's shape (the draw scales the vertical axis by the scene view's aspect),
// centred where that layout's centre and the widget's own move land in the column, it moves and scales every UI
// particle to the column's HUD at the band's scale, positions and sprite sizes together, whatever the shader did before
// (hud/overlay_band.h EffectViewport). The glow's design points and the
// effect's own sizes stay the game's; without the pass (a build whose call is not found) the glow is drawn where the
// game draws it. No engine call appends a viewport for this draw, so the replacement appends the game's own viewport
// command with the box (the command AllocBind appends, allocated as AllocBind allocates it), calls the game's draw, and
// appends the whole image again for the overlay broadcast that follows. Every conversion is made in player 1's column's
// layout (hud/overlay_layout.cpp answers the conversion's two size reads with the 1920x1080 frame), so an effect set
// once at its spawn, in whichever pass, is in the same layout as one set every tick. The engine's viewport command
// cannot start above or left of the image (its corner is unsigned), which a HUD size above 1 at a bottom or right corner
// needs: such a box is appended as a command of the mod's whose execute calls the command context's own float
// viewport and scissor setters (research/RE-ui-particle-negative-viewport.md), on a context the research read; on any
// other build it is cut at the image's edge (smaller, the particles' place in that column approximate). Each box is
// logged on change. The column is the one the UI pass just bound (hud/ui_targets.h), on the render thread; the boxes
// come from the overlay's rect call of the frame's last pass (game thread, through a seqlock).

namespace lss::platform {
class GameImage;
}

namespace lss::hud {
struct EffectBox;
}

namespace lss::hud::ui_effect_pass {

/// Checks the facts and installs the call redirect. False, with the reason logged, when a fact is not what the research
/// read.
bool Install(const platform::GameImage& image) noexcept;
/// Puts the engine's call back.
void Remove() noexcept;

/// Game thread, from the overlay's rect call of the frame's last pass: the viewport of one column's UI particles
/// (hud/overlay_band.h EffectViewport: a box's centre, width and height as fractions of the column's image), or none
/// when the band does not apply to that column this frame. The box may reach past the image's
/// right or bottom edge (the draw is clipped to the image).
void PublishColumnViewport(int column, const EffectBox& box, bool applies) noexcept;

/// Render thread, from the UI pass's bind: the column whose UI image the view draws now (a negative value for none).
void NoteUiPass(int column) noexcept;

/// The `hud-fx-pass` switch: the boxes on (the default) or off (the engine's whole-image viewport, for comparison).
void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

}  // namespace lss::hud::ui_effect_pass
