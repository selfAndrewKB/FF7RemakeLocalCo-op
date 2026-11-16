#pragma once

// Square Enix's ribbon trails, filled in ordinary memory and copied to the GPU in one pass (frame-rate work;
// research/RE-ribbon-fill-cost.md).
//
// The game builds every ribbon point's two vertices on the render thread, once per view, straight into the locked
// dynamic vertex buffer, and reads each position back from it to make the vertex's previous position. That buffer is
// write-combined memory, uncached for reads: each read-back waits on the bus, so an attack with many long trails (the
// Airbuster's mines and lasers) holds the render thread for every point of every trail in every view. The vertex
// fill's slot runs the game's own fill into a cached buffer of the mod's (per thread, grown on demand), then copies the
// whole result into the game's buffer in one sequential write: the same bytes, at memory speed. `ribbon-fill` off lets
// the game fill its buffer directly again, for an A/B in the same build.
// Each fill's time and vertices are counted either way, for the frame watch's lines and the frame capture's rows.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::render::ribbon_fill {

/// The ribbon vertex fills since install, counted on the render thread: their time in performance-counter ticks, the
/// vertices written, and the fills (one per ribbon emitter per view).
struct Totals
{
    std::uint64_t ticks    = 0;
    std::uint64_t vertices = 0;
    std::uint64_t fills    = 0;
};

/// Replaces the fill's slot if it holds the function the research read. False (logged) otherwise: the game fills its
/// buffer directly, uncounted.
bool Install(const platform::GameImage& image) noexcept;

/// Whether the slot is the mod's, so the totals count.
bool Installed() noexcept;

/// The totals so far. Any thread; each number is read on its own, so one taken while a fill ends may be one fill apart.
Totals Read() noexcept;

}  // namespace lss::render::ribbon_fill
