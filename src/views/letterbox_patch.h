#pragma once

// Two views in the game's Borderless mode: Square Enix's single-view letterbox switched off while they exist.
//
// In the game's Borderless and Fullscreen options (window mode 1) the final tonemap pass compares each view's size
// with the whole render target and, when they differ, fits the view into the target's middle (the game's own
// upscale of a lower option resolution). Two halves, or two columns of a spanned window, each differ from the
// target, so each is fitted over the other. The compare is `cmp dword [GSystemResolution.WindowMode], 1` and the
// compiler inlined it into the pass and each of its 320 pixel-shader setter permutations: 321 copies of one 7-byte
// instruction, each followed (after at most two moves) by a `jnz` over the fit. There is no single site to redirect
// (research/RE-view-size-fov.md §7.2, RE-multi-monitor-views.md §6.3; the user approved the patch 2026-10-20).
//
// Upstream of the pass (found live 2026-10-21, when the spanned window showed one view over both monitors with the
// 321 compares patched): ULocalPlayer::CalcSceneView asks the viewport's window mode and, in mode 1, replaces each
// view's rect and constrained rect in the init options by (0, 0, ResX, ResY), the whole resolution, keeping the
// player's rect aside. That is the first half of the upscale (one view at the option resolution; the pass then
// fits it); with two players every view covers the whole target and the last drawn wins. The same one-byte rewrite
// covers that `cmp eax, 1` (engine_facts kViewRectOverrideSite).
//
// While a further player is in, the mod rewrites the immediate of every copy from 1 to a value the mode never takes, so
// the `jnz` is always taken: the view rects stay the layout's and no view is fitted; when the last one leaves, the 1 goes
// back, so a solo option resolution below the display's is upscaled as before. Each write is one byte inside an
// instruction whose length does not change, so a thread running the compare sees the old or the new immediate, never
// a torn instruction. The sites are found at install by their bytes (the compare's displacement must name the
// window-mode global; the override by its 11-byte witness) and the patch refuses unless exactly 321 compares and the
// override are found. In the Windowed mode the compares are false either way, so the patch changes nothing there.
// Game thread.

#include <cstddef>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::views::letterbox_patch {

/// Finds the 321 compares. False (logged) if the count or the global differ; the patch is then unavailable and
/// split in Borderless keeps the game's letterbox.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick: the patch is on while a further player is in (and the switch is on), off otherwise.
void Tick(bool wanted) noexcept;

/// Puts every immediate back (unload).
void Restore() noexcept;

bool Applied() noexcept;

/// The `letterbox-patch.sites` reading: the count found and how many are patched now.
std::wstring Reading() noexcept;

}  // namespace lss::views::letterbox_patch
