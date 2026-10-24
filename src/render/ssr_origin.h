#pragma once

// Square Enix's screen-space reflections made to read each view's own pixels (the column-3 ghost in player 2's view).
//
// SE_RCPassSSR__Process draws every view's SSR over the whole buffer (stock 4.18 draws the view's rectangle), so the
// pixel shader's position is the absolute buffer pixel; the shader then adds the view's corner again before its
// GBuffer and depth loads. Player 1's corner is (0,0): correct. Player 2 one column in (x 2560 of 7680) reads column
// 3's surfaces at the same spot and traces them with its own camera: a faint copy of column 3's view on player 2's
// glossy floors and walls. Player 3 reads past the buffer and gets no reflections at all; in any two-view layout
// player 2 never had them. Each view's composite reads its own rectangle right after, so only the loads are wrong.
// Research: RE-third-view-ghost.md (shader, pass code, a capture's numbers).
//
// No setting moves the loads (the corner is a view constant the shader misuses), so this is a documented minimum
// fix, the bloom fix's way (render/bloom_origin.h): the pipeline-state call in each of the pass's two binders is
// redirected, and a pipeline whose pixel shader is an SSR shader gets a corrected copy of it in a copy of the
// pipeline description. The copy is made once per SSR shader, from the D3D12 shader object's own copy of its code,
// with the one doubled add swapped for a plain move (render/dxbc_fix.h; the same at corner 0, so right for every
// view and for single player), and created through the engine's own render-thread creation, from the render thread's
// own command list only. A shader is re-signed only after its stored checksum was reproduced; one without that exact
// instruction, once, is left as it is (logged). D3D11 keeps no code copy, so the fix stays off there (logged).
//
// While one family per player is drawn (render/one_family.h) every view starts at the corner, so the game's shader is
// already right: the pipeline calls inside a split frame keep it (render/family_now.h). The fix acts outside split
// frames (one view, `one-family off`, the split not running); it matters only with `one-family off`, kept for that.

namespace lss::platform {
class GameImage;
}

namespace lss::render::ssr_origin {

/// Verifies the two call sites, their target and the RHI (D3D12), and redirects the sites. False (logged) otherwise.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::render::ssr_origin
