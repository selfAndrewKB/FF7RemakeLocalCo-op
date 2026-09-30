#pragma once

// Square Enix's first bloom reduce made to read each view's own rows (the moving-content traces in player 2's half).
//
// SE_RCPassBloomReduce__Process, level 0, draws with a pixel shader that is given the source view rectangle
// (cb0[20] = Min.x, Min.y, W, H) but uses only its size: every view's bloom is built from the rows at (0,0). Player
// 1's view starts there, so it is correct; player 2's view (origin (0,540) top/bottom, (960,0) side by side) gets a
// blurred copy of player 1's picture, which shows wherever the two views differ. Stock UE4 hides this by switching
// bloom off for split screen (show-flag bit 0x2), which is why the split halves had no bloom at all.
// Research: RE-split-bit2-leak.md (the bug), RE-bloom-shader-patch.md (the corrected shader),
// RE-impl-facts-bloom-swap.md (the engine side).
//
// No setting or parameter moves the shader's reads (the origin is already passed; the shader drops it), so this is a
// documented minimum fix: a copy of that one shader with the origin added to its four sample positions
// (render/bloom_reduce_shader.h), created once through the engine's own shader creation, and used in place of the
// original for views that do not start at (0,0):
//   - the two level-0 binder calls are redirected to note the pass's view for the length of the call (render thread);
//   - the pipeline-state call inside each binder is redirected: when the noted view does not start at (0,0) and the
//     pipeline's pixel shader is the original (checked by the engine's own shader hash, which both RHIs keep), the
//     call gets a copy of the pipeline description with the corrected shader in it. The engine's pipeline cache keys
//     on that description, so the corrected pipeline is built once and cached like any other. Constants, textures
//     and draws stay the game's.
// The corrected shader's code blob is the corrected DXBC in the engine's framing of the original's (resource tables
// before it, optional data and footer after it, read once from the D3D12 shader object's copy of the code), so the
// same bytes serve D3D12 and D3D11.

namespace lss::platform {
class GameImage;
}

namespace lss::render::bloom_origin {

/// Verifies the four call sites, their targets and the RHI, and redirects the sites. False (logged) if anything
/// differs or the RHI is neither D3D12 nor D3D11; bloom must then stay off in split (show-flag bit 0x2).
bool Install(const platform::GameImage& image) noexcept;

/// True once installed: player 2's bloom reads its own rows, so the split rule's bloom bit may be forced on.
bool Installed() noexcept;

/// On by default; off leaves the original shader in every view (for A/B checks). Any thread.
void SetEnabled(bool enabled) noexcept;
bool Enabled() noexcept;

}  // namespace lss::render::bloom_origin
