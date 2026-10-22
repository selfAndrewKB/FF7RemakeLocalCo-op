#pragma once

// Battle world markers placed through each player's own camera (RE-hud-battle-markers.md §3.1, §3.3; Rebirth's
// enemy_gauges and popups, the same capture-and-replay shape).
//
// The HUD ticks run once a frame, before the overlay renders, and place every marker through player 1's view. Their
// placement functions are stateless (they project, then write the widget's viewport offsets), so the mod records each
// call's arguments during the game's own tick and makes the same calls again around player 2's pass:
//   - enemy bars (slot 27): EndUISlot27_EnemyBar_PlaceUntargeted 0x1413DBBC0 and _PlaceTargeted_LeaderClamp 0x1413DC010,
//     which project with the bar widget's PlayerContext (UUserWidget+0x1E8: player 2's local player for the replay);
//     the displayed target's bar then gets its y-ease applied (SetPositionInViewport, the ease not stepped);
//   - battle text and damage numbers (slot 16): EndUI_BattleText_PlaceAtWorld_StoredPC 0x141496AD0, which projects with
//     the controller it is given (player 2's for the replay).
// The entries are redirected (platform/entry_redirect.h; their first two instructions are copied, so the redirects are
// installed from the game thread, outside these functions, which only the game thread runs).
// Order each frame (hud/second_image.h), per further column, the highest first: its host set, its replay, its render;
// then column 0's host set, column 0's replay (the tick's own inputs: it reproduces the tick), player 1's render. Not
// replayed: the bars' timers, eases and scale, the battle text's count-up and its UI effects (player 1's point in every
// column).

namespace lss::platform {
class GameImage;
}

namespace lss::hud::marker_replay {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Game thread, every tick, outside the HUD: installs the entry redirects once.
void Tick() noexcept;

/// Game thread, from the overlay driver: places every marker recorded this frame for column `column`
/// (hud/overlay_pass.h; 0 = player 1's, with the tick's own inputs), whose host set must already be in the overlay.
/// `localPlayer` is that column's ULocalPlayer, `controller` its player controller.
void Replay(int column, void* localPlayer, void* controller) noexcept;

/// Game thread, after the frame's overlay render: forgets this frame's records.
void EndFrame() noexcept;

/// The `hud-markers` switch (on at start).
void SetEnabled(bool on) noexcept;
bool Enabled() noexcept;

}  // namespace lss::hud::marker_replay
