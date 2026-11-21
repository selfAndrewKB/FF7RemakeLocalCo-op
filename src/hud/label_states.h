#pragma once

// A seat's own laid-out state of a shared text block, one book per seat (research/RE-per-player-icons.md).
//
// One Square Enix text block (SEndTextBlock) is painted in every view. Where each player needs their own result from it
// (the button prompts' art, hud/button_art), the block keeps one state per player: around the seat's HUD pass the seat's
// last state is swapped into it (player 1's set aside), and after the pass player 1's goes back in (the seat's set aside
// until the next pass). The bytes are the game's own relayout output, moved bitwise as the game itself moves these
// array headers; a copy set aside is freed with the game's own destructor helper.
//
// This header is the bookkeeping only (records, the reset when what the state depends on changes, dead widgets),
// reached through LabelAccess so it is tested on the host. Game thread only: the block's reference counts are not
// thread-safe.

#include "engine_facts.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::hud::label_states {

// The swapped ranges of an SEndTextBlock; everything else stays in the widget.
inline constexpr std::uint32_t kRunsOffset    = facts::kSlateTextRuns;   // glyph pages, desired and allotted size, width
inline constexpr std::uint32_t kRunsSize      = facts::kSlateTextRunsBytes;
inline constexpr std::uint32_t kFlagsOffset   = facts::kSlateTextFlags;  // relayout pending, glyph characters, button tag
inline constexpr std::uint32_t kFlagsSize     = facts::kSlateTextFlagsBytes;
inline constexpr std::uint32_t kQuadsOffset   = facts::kSlateTextQuads;  // three quad arrays and the cached shadow offset
inline constexpr std::uint32_t kQuadsSize     = facts::kSlateTextQuadsBytes;
inline constexpr std::uint32_t kShadowInQuads = kQuadsSize - 2 * sizeof(float);  // 1.0f, 1.0f in an empty block

/// One state set aside, laid out range by range (each array header 8-aligned, as in the widget).
struct alignas(8) Parked
{
    alignas(8) std::byte runs[kRunsSize];
    alignas(8) std::byte flags[kFlagsSize];
    alignas(8) std::byte quads[kQuadsSize];
};

/// The state the block's constructor writes: what the game's first relayout starts from.
Parked EmptyState() noexcept;

/// The engine side. Every function is called on the game thread.
struct LabelAccess
{
    bool (*usable)(void* widget) noexcept;                // the widget may carry a seat's state
    bool (*alive)(void* controller) noexcept;             // the shared count above 0 (the mod holds a weak reference)
    bool (*swap)(void* widget, Parked& parked) noexcept;  // exchanges the ranges; false if it faulted (nothing changed)
    std::uint64_t (*stamp)(void* widget) noexcept;        // changes with what the state depends on outside its ranges
    bool (*holdWeak)(void* controller) noexcept;          // keeps the block's memory readable after the widget dies
    void (*releaseWeak)(void* controller) noexcept;       // the game's own release (frees the block at zero)
    void (*freeParked)(Parked& parked) noexcept;          // the game's destructor helper on a set-aside state
};

struct LabelCounts
{
    std::uint64_t swapsIn = 0, swapsOut = 0, created = 0, resets = 0, refused = 0, full = 0, faults = 0;
    std::uint64_t diedInside = 0, diedAside = 0, released = 0;
};

class LabelBook
{
public:
    static constexpr std::size_t kMaxLabels = 256;

    /// `fresh`: the state a new record, or one whose stamp changed, starts from.
    explicit LabelBook(LabelAccess access, Parked fresh = EmptyState()) noexcept : access_(access), fresh_(fresh) {}

    /// Puts the seat's state into `widget` (its controller block is `controller`) before a seat's HUD pass. False only
    /// if the swap faulted (the caller then stops the feature).
    bool SwapIn(void* widget, void* controller) noexcept;

    /// After the seat's paint: player 1's state back into every block that has the seat's, each record stamped first.
    void SwapBack() noexcept;

    /// Outside the passes: drops the records of widgets that died (their set-aside state freed).
    void Sweep() noexcept;

    /// The seat out, or the mod leaving: player 1's state in every block, every record freed.
    void ReleaseAll() noexcept;

    std::size_t        Count() const noexcept { return count_; }
    bool               AnyInside() const noexcept;
    /// The seat's state is in the block whose controller is `controller`.
    bool               Holds(const void* controller) const noexcept;
    const LabelCounts& Counts() const noexcept { return counts_; }

private:
    struct Record
    {
        void*         widget     = nullptr;
        void*         controller = nullptr;
        std::uint64_t stamp      = 0;
        bool          inside     = false;  // the seat's state is in the widget, player 1's in `parked`
        Parked        parked{};
    };

    Record* Find(const void* controller) noexcept;
    void    Drop(std::size_t index) noexcept;  // frees the set-aside state and the weak reference

    LabelAccess                    access_;
    Parked                         fresh_;
    std::array<Record, kMaxLabels> records_{};
    std::size_t                    count_ = 0;
    LabelCounts                    counts_;
};

}  // namespace lss::hud::label_states
