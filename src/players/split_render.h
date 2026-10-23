#pragma once

// The single-player look in each half: the effects stock UE 4.18 switches off for a split viewport.
//
// UGameViewportClient::Draw passes "split screen" to EngineShowFlagOverride, which clears the TemporalAA show
// flag (and two more bits, most likely Bloom and MotionBlur) for every view; Square Enix's anti-aliasing setup
// then picks no anti-aliasing at all, so dithered hair and foliage stay raw in both halves. Each player's view
// already has its own view state and history. The engine's own lever is the show-flag force mask its override
// applies last (a global only its static initialiser writes, zero): while player 2 is in, it holds exactly the
// bits the split rule clears, and it goes back to zero at the leave. Research: RE-render-views.md.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::split_render {

/// Resolves the mask. False (logged) if it is missing or not zero (something else uses it).
bool Install(const platform::GameImage& image) noexcept;

/// A further local player joined (a seat, a mirror): force the split-disabled effects back on (the configured mask)
/// before its first frame. Game thread.
void OnJoin() noexcept;
/// Each engine tick, after the session's and the mirrors': `furtherViews` whether any local player beyond player 1
/// is in. Forces the mask while one is, puts it back to zero (if it still holds what the mod wrote) once none is;
/// a count-free reconcile, so a seat leaving beside a mirror keeps the mirror's view whole. Game thread.
void Tick(bool furtherViews) noexcept;

/// The bits forced on while player 2 is in. Default 0x1008 (TAA and 0x1000); the mod raises it to 0x100A (bloom too)
/// when render/bloom_origin installs, since bloom leaks player 1's rows into player 2's half without it. Applied at
/// once if player 2 is in. Game thread.
void SetMask(std::uint32_t mask, bool playerTwoIn) noexcept;
std::uint32_t Mask() noexcept;

}  // namespace lss::players::split_render
