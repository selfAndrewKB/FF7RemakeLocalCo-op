#pragma once

// Square Enix's text block (SEndTextBlock), the Slate widget behind every game text, as the mod's per-player states read
// and move it (research/RE-per-player-icons.md).
//
// A block lays its text out once (its relayout: glyph pages and paint quads, button tags turned into the art of its
// font's device bytes) and paints the quads until the next relayout. hud/label_states keeps a player's own laid-out
// state of a shared block; this module is that book's engine side: which widgets can carry such a state, the exchange
// of the state's ranges, the game's own destructor helper and reference counts, the game's relayout, and the game's set
// of every live block. Found once (Install, idempotent); every call on the game thread (the widgets are its).

#include "hud/label_states.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::text_blocks {

/// Finds the block's class, its destructor helper, the relayout and the live set. False if any is not what the research
/// read (logged). Idempotent.
bool Install(const platform::GameImage& image) noexcept;

/// The block's MakeShared reference controller (just before the widget).
void* ControllerOf(void* block) noexcept;

/// Why `block` cannot carry a player's own state (not an SEndTextBlock, not in its own MakeShared block, no shared
/// reference left), or null when it can.
const wchar_t* Unswappable(void* block) noexcept;

/// The block whose controller this is still has a shared reference (the mod holds a weak one).
bool Alive(void* controller) noexcept;

/// Exchanges the block's laid-out state with `parked`. Every read comes before any write: false (nothing changed) if
/// one faulted.
bool SwapLayout(void* block, label_states::Parked& parked) noexcept;

/// What a set-aside layout depends on in the block outside the swapped ranges: both fonts.
std::uint64_t FontStamp(void* block) noexcept;

/// The mod's weak reference on the block's controller: the block's memory stays readable after it dies.
bool HoldWeak(void* controller) noexcept;
void ReleaseWeak(void* controller) noexcept;  // the game's own release (frees the block at zero)

/// The destructor's own helper on a set-aside state; then it is the empty state again.
void FreeParked(label_states::Parked& parked) noexcept;

/// The game's relayout of `block` (under the block's layout lock it only marks itself pending). False if it faulted.
bool Relayout(void* block) noexcept;

/// Calls `visit(block)` for every live block until it returns false (each a live block at its visit: the game's set
/// holds a block from its constructor's end to its destructor's start). Nothing before Install. Does not throw.
void ForEachLive(bool (*visit)(void* block, void* context) noexcept, void* context) noexcept;

}  // namespace lss::hud::text_blocks
