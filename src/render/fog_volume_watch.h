#pragma once

// A watch on each view's fog volumes (RE-haze-occlusion.md, Pass 4): logging only, for the blue haze in players 2's
// and 3's views.
//
// Volumetric fog is shaped by fog-volume primitives (meshes with a volume material). Each view keeps its own list of
// them, filled from that view's visibility after occlusion culling, and adds only those to its own fog
// (VoxelizeFogVolumePrimitives also skips an entry whose visibility bit is clear). A fog volume wrongly culled in a
// view would leave that view's fog without it while the other views keep it [ASSUMPTION; this watch tests it]. The
// haze needs culling (turning it off removes it) yet the per-object culling records read the same hazy and clean,
// so the watch reads the lists the fog itself is built from, every frame.
//
// It passes through the one call that builds every view's fog (ComputeVolumetricFog, two call sites in the frame),
// reads each view's list and which entries the fog will take (their visibility bit; whether the fog itself runs
// for that view is not checked), and changes nothing. Only draws of two or more views are read: captures have one.
// Logged: the install (Info), a summary each second with the views' counts and the fog volumes that came and went
// (Debug, only when something changed), each change (Trace). Reading `fog-volumes`: the last summary.

namespace lss::platform {
class GameImage;
}

namespace lss::render::fog_volume_watch {

/// Verifies the function and both call sites and redirects them (a pass-through). False (logged) otherwise.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::render::fog_volume_watch
