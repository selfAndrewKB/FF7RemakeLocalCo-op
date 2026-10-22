#pragma once

// The columns' layer entries on the render thread (RE-hud-two-images.md §3, §7: H2 and H3).
//
// Pool 2's ring slot holds [the further columns' entries, player 1's entry] while a further column has an image
// (hud/second_image.h). The render thread draws the slot once per view (hud/ui_targets.h gives each view its own
// target), so:
//   - each entry is drawn only in its own column's view: the call to FEndMenuRenderer__DrawEntryBatchToTarget
//     (0x1439FCFD8) is redirected and skipped for another column's entry (overlay_pass::DrawSkipped). Uploads stay
//     (they run before it, and only on a fresh consume), so whichever view consumes the slot uploads every entry;
//   - column 1's entries upload into and draw from a vertex/index buffer pair of their own (hud/image_code.h at
//     0x143A000E2), so a view that reuses the slot draws each entry from its own last upload.
// The pair is made on the render thread the way FEndMenuRenderer's constructor makes pool 2's (64-byte resources with
// the game's vtables, capacities 1024/1536, InitResource), once, and never freed (the renderer lives as long as the
// game). The game grows them as it grows its own. No game path makes a second pair: documented in
// RE-hud-two-images.md §7 as the one recreation of this step.
// One pair and one entry table serve one further column (column 1); a third view needs a second pair, built with it
// (plan/seats.md step 5). Until then a second further column's entry is not gated (logged once) and a view beyond
// column 1 draws as column 1's view.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::image_entries {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread: asks the render thread for column 1's buffer pair (made at the next UI pass). True once made.
bool BuffersReady() noexcept;
void RequestBuffers() noexcept;

/// Game thread, right after FindOrAdd returned an entry in column `column`'s pass (hud/overlay_pass.h): records it (one
/// per ring slot), so the render thread draws it only in that column's view.
void RecordEntry(int column, void* entry) noexcept;

/// Render thread, from hud/ui_targets.h at each view's UI pass: which column's view is drawn now (-1: no split view
/// identity, nothing is gated), and makes the buffers if asked.
void BeginUiPass(int column) noexcept;
/// Render thread, after the UI passes (the first post-process): no view is being drawn any more.
void EndUiPasses() noexcept;

}  // namespace lss::hud::image_entries
