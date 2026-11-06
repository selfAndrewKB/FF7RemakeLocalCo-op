#pragma once

// Every player's view keeps the motion of moving things: the renderer's table of where each moving object was last frame
// steps once a frame, as with one player, not once per player's family (research/RE-velocity-per-family.md).
//
// Why: TAA lines each frame up with the last one by each object's motion since then, which the velocity pass reads from
// the scene's motion table (stock 4.18 FMotionBlurInfoData, FScene+0x6350). Every submission steps the table twice:
// StartFrame, queued by the submission's EnsureMotionBlurCacheIsUpToDate behind the scene's started flag (FScene+0x6488,
// which every submission clears at its end), copies last frame's places into the slot the velocity pass reads; then
// UpdateMotionBlurCache, at the start of the family's draw and skipped while the table's paused byte (FScene+0x63A1) is
// set, records this frame's places and drops the entries that did not move. With one family per player
// (render/one_family.h) the second family's update drops every entry, so players 2 and 3 and the mirror columns draw
// moving objects without their motion through the world: smear, and dithered hair, when the camera follows a moving
// character [CONFIRMED code; seen in player 3's view].
//
// How, with the game's own two gates, for every family after the frame's first:
//   1. game thread, once the family's boundary is queued and right before its submission: the started flag = 1, the
//      value the game's Ensure sets itself, so the submission queues no StartFrame; its own tail clears the flag again;
//   2. render thread, in the family's boundary task (render/family_boundary.h): the paused byte = 1, so the family's
//      UpdateMotionBlurCache returns at once; after the last family the byte goes back to the family's bWorldIsPaused
//      (+0x84), the value the first family's StartFrame wrote.
// Every family then reads the table as the first one did, as one player's frame does. The family's scene is checked by
// its vtable (FScene's GetRenderScene returns the scene itself, so it is the object the submission uses), or nothing is
// written. GPU particles need nothing: the later families already simulate them with zero time (render/one_family.h).
//
// Switch `motion-history on | off` (on at start): off leaves the table stepping once per family, for an A/B. The reading
// gives the switch, the split frames whose later families were armed, and the families refused.

#include "render/family_boundary.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::render::motion_history {

/// Verifies the scene's vtable and the gate sites. False (logged) otherwise: the table steps once per family.
bool Install(const platform::GameImage& image) noexcept;

/// What one family's submission needs, decided on the game thread.
struct Step
{
    family_boundary::ByteWrite render;            // the paused byte, written in the family's boundary task
    std::uint8_t*              started = nullptr; // the scene's started flag, set by Arm; null: the game's StartFrame runs
};

/// Game thread: the step for view k of a split frame (k = 0..n-1), or for after the frame's last family (k = -1, which
/// puts the paused byte back only if a family of the frame was armed). None for view 0, with the switch off, when not
/// installed, or when the family's scene is not an FScene.
Step For(const std::uint8_t* family, int k) noexcept;

/// Game thread, once the step's boundary is queued and right before the family's submission: sets the started flag.
void Arm(const Step& step) noexcept;

}  // namespace lss::render::motion_history
