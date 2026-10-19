#pragma once

// Player 2's layer entries on the render thread (RE-hud-two-images.md §3, §7: H2 and H3).
//
// Pool 2's ring slot holds [player 2's entry, player 1's entry] while player 2 has an image (hud/second_image.h). The
// render thread draws the slot once per view (hud/ui_targets.h gives each view its own target), so:
//   - each entry is drawn only in its own view: the call to FEndMenuRenderer__DrawEntryBatchToTarget (0x1439FCFD8) is
//     redirected and skipped for the other player's entry. Uploads stay (they run before it, and only on a fresh
//     consume), so whichever view consumes the slot uploads both entries;
//   - player 2's entries upload into and draw from a vertex/index buffer pair of their own (hud/image_code.h at
//     0x143A000E2), so a view that reuses the slot draws each entry from its own last upload.
// Player 2's pair is made on the render thread the way FEndMenuRenderer's constructor makes pool 2's (64-byte
// resources with the game's vtables, capacities 1024/1536, InitResource), once, and never freed (the renderer lives as
// long as the game). The game grows them as it grows its own. No game path makes a second pair: documented in
// RE-hud-two-images.md §7 as the one recreation of this step.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::image_entries {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread: asks the render thread for player 2's buffer pair (made at the next UI pass). True once made.
bool BuffersReady() noexcept;
void RequestBuffers() noexcept;

/// Game thread, right after FindOrAdd returned an entry in player `player`'s pass (1 or 2): records it (one per ring
/// slot), so the render thread draws it only in that player's view.
void RecordEntry(int player, void* entry) noexcept;

/// Render thread, from hud/ui_targets.h at each view's UI pass: which player's view is drawn now (-1: no split view
/// identity, nothing is gated), and makes the buffers if asked.
void BeginUiPass(int player) noexcept;
/// Render thread, after the UI passes (the first post-process): no view is being drawn any more.
void EndUiPasses() noexcept;

}  // namespace lss::hud::image_entries
