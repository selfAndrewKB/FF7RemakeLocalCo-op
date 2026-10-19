#pragma once

// A pad's own copy of the game's action state (RE-input.md §1.6-§1.7, RE-input-p2.md §2.3).
//
// Square Enix's input object keeps every action in two TSets of 40-byte elements (an FName key, then at +0x18
// four state bytes: held last frame, held now, repeat, changed-this-frame latch): the raw table the gameplay
// pollers read and a UI-filtered twin. The game writes them from its key handlers (the pad is known there and
// nowhere later), advances `prev = held; latch = 0` once per frame in its input tick, and keeps no per-pad
// state: a second pad's release lets go of the first pad's held action.
//
// A View is one pad's copy of those bytes (both tables and the 8-byte d-pad field). While it is exchanged
// with the live bytes, the game's own handlers write the copy with their exact rules, and the game's own
// pollers read it. The only rule restated here is the tick's two-line advance, made on each copy after the
// game's tick. The element sets are fixed after the game's init (the handlers only look elements up), so a
// view records each table's element names once and is rebuilt if the table ever changes shape.
//
// Pure functions over the owner's bytes: no engine dependency, so the host test exercises them on a fake
// owner (tests/action_tables_test.cpp).

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lss::players::action_tables {

inline constexpr std::uint32_t kElementBytes = 40;
inline constexpr std::uint32_t kElementState = 0x18;  // S0 prev, S1 held, S2 repeat, S3 latch
inline constexpr std::uint32_t kDpadBytes    = 8;

// TSet<Elem40> = TSparseArray (TArray data: ptr +0, num +8, max +0xC; TBitArray: 4 inline u32 +0x10, secondary
// +0x20, NumBits +0x28, MaxBits +0x2C; FirstFreeIndex +0x30; NumFreeIndices +0x34) then the hash (UE 4.18 layout,
// matching the game's own tick: data at set+0, bits at set+0x10, secondary at set+0x20, NumBits at set+0x28).
inline constexpr std::uint32_t kSetData      = 0x00;
inline constexpr std::uint32_t kSetNum       = 0x08;
inline constexpr std::uint32_t kSetBits      = 0x10;
inline constexpr std::uint32_t kSetSecondary = 0x20;
inline constexpr std::uint32_t kSetNumBits   = 0x28;
inline constexpr std::uint32_t kSetNumFree   = 0x34;

using State = std::array<std::uint8_t, 4>;

/// The live layout: where the two tables and the d-pad field sit in the owner.
struct Layout
{
    std::uint32_t raw  = 0;  // the raw table (the pollers')
    std::uint32_t ui   = 0;  // the UI-filtered twin
    std::uint32_t dpad = 0;  // 8 bytes of real d-pad bits
};

/// One table's shape as recorded: an FName per index (0 for a free index).
struct Shape
{
    std::vector<std::uint64_t> names;
    std::int32_t               numFree = 0;
};

/// One pad's copy of the state.
struct View
{
    Shape              rawShape, uiShape;
    std::vector<State> raw, ui;  // indexed like the tables (free indices unused)
    std::array<std::uint8_t, kDpadBytes> dpad{};
    bool               built = false;
};

namespace detail {

template <class T>
T At(const std::uint8_t* base, std::uint32_t offset) noexcept
{
    T value;
    std::memcpy(&value, base + offset, sizeof(T));
    return value;
}

/// Whether index `i` of the sparse array at `set` is allocated (its bit in the inline or secondary words).
inline bool Allocated(const std::uint8_t* set, std::int32_t i) noexcept
{
    const auto numBits = At<std::int32_t>(set, kSetNumBits);
    if (i < 0 || i >= numBits)
    {
        return false;
    }
    const auto* words = At<const std::uint32_t*>(set, kSetSecondary);
    if (words == nullptr)
    {
        words = reinterpret_cast<const std::uint32_t*>(set + kSetBits);
    }
    return (words[i >> 5] >> (i & 31)) & 1u;
}

inline std::uint8_t* Element(const std::uint8_t* set, std::int32_t i) noexcept
{
    return At<std::uint8_t*>(set, kSetData) + static_cast<std::size_t>(i) * kElementBytes;
}

/// Records the table's shape; false if it has no data.
inline bool Record(const std::uint8_t* set, Shape& shape) noexcept
{
    const auto num = At<std::int32_t>(set, kSetNum);
    if (num <= 0 || At<std::uint8_t*>(set, kSetData) == nullptr)
    {
        return false;
    }
    shape.names.assign(static_cast<std::size_t>(num), 0);
    for (std::int32_t i = 0; i < num; ++i)
    {
        if (Allocated(set, i))
        {
            shape.names[static_cast<std::size_t>(i)] = At<std::uint64_t>(Element(set, i), 0);
        }
    }
    shape.numFree = At<std::int32_t>(set, kSetNumFree);
    return true;
}

/// The cheap check made at every exchange: the table still has the recorded count and free count. (The
/// names are compared only when a view is built; the handlers never add or remove elements.)
inline bool SameShape(const std::uint8_t* set, const Shape& shape) noexcept
{
    return At<std::int32_t>(set, kSetNum) == static_cast<std::int32_t>(shape.names.size()) &&
           At<std::int32_t>(set, kSetNumFree) == shape.numFree;
}

inline void Exchange(const std::uint8_t* set, const Shape& shape, std::vector<State>& copy) noexcept
{
    for (std::size_t i = 0; i < shape.names.size(); ++i)
    {
        if (shape.names[i] != 0)
        {
            State* live = reinterpret_cast<State*>(Element(set, static_cast<std::int32_t>(i)) + kElementState);
            std::swap(*live, copy[i]);
        }
    }
}

}  // namespace detail

/// Builds (or rebuilds) `view` from the owner's tables, all state zero (nothing held). False if a table is empty.
inline bool Build(const std::uint8_t* owner, const Layout& layout, View& view) noexcept
{
    view.built = false;
    if (!detail::Record(owner + layout.raw, view.rawShape) || !detail::Record(owner + layout.ui, view.uiShape))
    {
        return false;
    }
    view.raw.assign(view.rawShape.names.size(), State{});
    view.ui.assign(view.uiShape.names.size(), State{});
    view.dpad.fill(0);
    view.built = true;
    return true;
}

/// Whether the owner's tables still have the shape `view` was built on.
inline bool Fits(const std::uint8_t* owner, const Layout& layout, const View& view) noexcept
{
    return view.built && detail::SameShape(owner + layout.raw, view.rawShape) &&
           detail::SameShape(owner + layout.ui, view.uiShape);
}

/// Swaps the view's bytes with the live ones (both tables and the d-pad field). Called twice: in and out.
inline void Exchange(std::uint8_t* owner, const Layout& layout, View& view) noexcept
{
    detail::Exchange(owner + layout.raw, view.rawShape, view.raw);
    detail::Exchange(owner + layout.ui, view.uiShape, view.ui);
    std::array<std::uint8_t, kDpadBytes> live{};
    std::memcpy(live.data(), owner + layout.dpad, kDpadBytes);
    std::memcpy(owner + layout.dpad, view.dpad.data(), kDpadBytes);
    view.dpad = live;
}

/// The game's per-frame advance, on a view that is not exchanged in: `if (!latch) prev = held; latch = 0`.
inline void Advance(View& view) noexcept
{
    for (auto* table : {&view.raw, &view.ui})
    {
        for (State& s : *table)
        {
            if (s[3] == 0)
            {
                s[0] = s[1];
            }
            s[3] = 0;
        }
    }
}

}  // namespace lss::players::action_tables
