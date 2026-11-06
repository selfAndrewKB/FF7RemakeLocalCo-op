#pragma once

// Which UI target each view samples, with N columns (hud/ui_targets.h). Pure, host-tested
// (tests/target_rotation_test.cpp): the render thread's bookkeeping without the engine calls.
//
// The game keeps ONE pooled UI target in a member of its scene targets. Each view's UI pass draws the HUD into
// whatever the member names (allocating a target from the pool when it is null), and each view's final pass samples
// whatever the member names when its parameters are set. So with several views the last UI pass wins everywhere.
// The rule: at a further column's UI pass the member's target is moved out (the reference moves with it: the mod
// holds it), so the game's allocator hands that pass a target of its own; at each view's post-process the member is
// made to name the target that view should sample, the one it had before going back to the held set; after the last
// view the mod releases every target it still holds, so it owns no reference across Renders (the member's own stays
// the game's, released at the next Render start). A mirror's column has no HUD image of its own (hud/overlay_pass.h:
// the mirror's columns show column 0's image): its pass still gets its own target, so it does not draw over the
// previous column's, and its final pass samples column 0's.

#include "hud/overlay_pass.h"

namespace lss::hud::target_rotation {

inline constexpr int kMaxColumns = overlay_pass::kMaxColumns;

/// One Render's targets by column and which of them the mod holds a reference to.
class Rotation
{
public:
    /// A new Render: nothing listed, nothing held (the caller released what was held).
    constexpr void Begin() noexcept { *this = Rotation{}; }

    /// Column `column`'s UI pass is about to run with the member naming `member` (null when the game released it).
    /// True when the member's target is to be moved out first (the caller then sets the member null): a further
    /// column not listed yet, the moves enabled, column 0's pass seen, and the member naming a target of a column
    /// listed so far. A column passed again keeps its first target and moves nothing (the engine's loop runs each
    /// view once; the guard keeps the "in the member or held once" invariant whatever the loop does).
    constexpr bool BeforeUiPass(int column, const void* member, bool enabled) noexcept
    {
        if (column <= 0 || column >= kMaxColumns || !enabled || targets_[0] == nullptr || member == nullptr ||
            targets_[column] != nullptr)
        {
            return false;
        }
        const int owner = OwnerOf(member);
        if (owner < 0)
        {
            return false;
        }
        held_[owner] = true;
        return true;
    }

    /// Column `column`'s UI pass ran; the member names `target` now (that column's own, or a shared one). A column
    /// listed already keeps its first target.
    constexpr void AfterUiPass(int column, void* target) noexcept
    {
        if (column < 0 || column >= kMaxColumns || targets_[column] != nullptr)
        {
            return;
        }
        targets_[column] = target;
        count_           = column + 1 > count_ ? column + 1 : count_;
    }

    /// Column `column`'s post-process is about to run with the member naming `member`; `playerColumns` the columns
    /// with a HUD image of their own (0 .. playerColumns-1), the rest the mirrors'. The target the member must name
    /// for this view, or null when it already does (or the column is unknown): the caller sets the member to it.
    constexpr void* BeforePostProcess(int column, const void* member, int playerColumns) noexcept
    {
        if (column < 0 || column >= kMaxColumns)
        {
            return nullptr;
        }
        void* wanted = column < playerColumns ? targets_[column] : targets_[0];
        if (wanted == nullptr || wanted == member)
        {
            return nullptr;
        }
        if (const int owner = OwnerOf(member); owner >= 0)
        {
            held_[owner] = true;  // the member's target goes back to the held set
        }
        held_[OwnerOf(wanted)] = false;  // the wanted one leaves it (its owner exists: it is a listed target)
        return wanted;
    }

    /// The targets the mod still holds after the last view, each once, to release; the record is then empty.
    constexpr int TakeHeld(void* (&out)[kMaxColumns]) noexcept
    {
        int n = 0;
        for (int c = 0; c < kMaxColumns; ++c)
        {
            if (held_[c] && targets_[c] != nullptr && !Listed(out, n, targets_[c]))
            {
                out[n++] = targets_[c];
            }
        }
        Begin();
        return n;
    }

    constexpr int   Count() const noexcept { return count_; }
    constexpr void* Target(int column) const noexcept { return column >= 0 && column < kMaxColumns ? targets_[column] : nullptr; }
    constexpr bool  Held(int column) const noexcept { return column >= 0 && column < kMaxColumns && held_[column]; }

private:
    /// The first column whose target is `target`, or -1.
    constexpr int OwnerOf(const void* target) const noexcept
    {
        for (int c = 0; c < kMaxColumns; ++c)
        {
            if (targets_[c] != nullptr && targets_[c] == target)
            {
                return c;
            }
        }
        return -1;
    }

    static constexpr bool Listed(void* const (&out)[kMaxColumns], int n, const void* target) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            if (out[i] == target)
            {
                return true;
            }
        }
        return false;
    }

    void* targets_[kMaxColumns] = {};
    bool  held_[kMaxColumns]    = {};
    int   count_                = 0;
};

}  // namespace lss::hud::target_rotation
