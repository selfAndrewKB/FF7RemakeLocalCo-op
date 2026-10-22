#pragma once

// The field objective and item icons (the diamond with its line, FieldQuestIcon_C) placed through each player's own
// camera (RE-field-icon-placement.md §2, §5.1; Rebirth's hud/area_icons_two built a second icon manager, here one canvas
// serves both views because the placement is one stateless call per icon).
//
// The slot-51 HUD tick places every icon once a frame with the first player controller (player 1's view), and both HUD
// passes paint the one canvas, so player 2's column showed player 1's placement. The placement function
// (EndUISlot51_QuestIcon_PlaceEntry_FadeByDt) takes the controller and the leader's location as arguments and keeps no
// state beyond the icon's alpha (faded by dt) and, when the point is behind the camera, the slot's last offset. So the
// mod records each call during the tick and makes it again per pass: for player 2 with player 2's controller, player 2's
// member's location and player 2's own alpha swapped into the entry; for player 1 with the tick's inputs and dt 0, which
// reproduces the tick. Each view keeps the last offset its own projection wrote and re-writes it on the frames its
// projection fails, so a fading icon stays where that view last saw it.
// Entry redirect (platform/entry_redirect.h): the first three instructions (5 bytes) are copied, so the redirect is
// installed from the game thread, outside the function, which only the game thread runs.
// Order each frame (hud/second_image.h): right after the battle markers' replay for the same pass, the pass's host set in.
// Player 2's placement also writes the shared widget's colour, visibility and the entry's memoryless state fields;
// player 1's placement, last, writes them back as the tick did. All state is the game thread's (the records of a frame,
// each view's alpha and last offset per icon).

namespace lss::platform {
class GameImage;
}

namespace lss::hud::field_icons {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread, every tick, outside the HUD: installs the entry redirect once; `memberTwo` is player 2's character
/// (its location is the icons' range gate for player 2's view), nullptr while player 2 is out.
void Tick(void* memberTwo) noexcept;

/// Game thread, from the overlay driver: places every icon recorded this frame for `player` (1 or 2), whose host set
/// must already be in the overlay. `controller` is player 2's player controller; player 1's pass uses the tick's own.
void Replay(int player, void* controller) noexcept;

/// Game thread, after the frame's overlay render: forgets this frame's records.
void EndFrame() noexcept;

/// The `hud-field-icons` switch (on at start).
void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

}  // namespace lss::hud::field_icons
