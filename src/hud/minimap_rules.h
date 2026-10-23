#pragma once

// The navi map's pool-0 handover across the frame's HUD passes, as a pure rule (hud/minimap_two.h holds the game
// side; the seats follow plan/seats.md step 3).
//
// The menu renderer's pool 0 turns once a frame: one slot acquired, one submitted, or the next acquire landing on an
// unsubmitted slot spins. The HUD renders once per column (hud/overlay_pass.h: the further columns first, the highest
// first; column 0, player 1's, the game's own render, last), and each column's pass paints that column's octagon
// (player 1's in column 0, a seat's in its own column). So the frame's first octagon to acquire in a further pass takes
// the slot and holds it; every later pass's octagon, the next further column's or player 1's, is handed the held slot
// and adds its own entry; the submit of the held slot is skipped in a further pass and sent in column 0's, where it
// carries every column's entry. Another painter (a second map widget, an octagon in a pass not its own) gets the
// game's acquire and submit untouched, so no slot is stranded. A second acquire by the column that holds the slot
// (two paints in one pass) sends the held slot first, then acquires anew; a slot still held when the frame ends is
// sent by the caller. No game, no threads: host-tested in tests/minimap_rules_test.cpp.

namespace lss::hud::minimap_rules {

class PoolHandover
{
public:
    enum class Take
    {
        Game,               // the game's own acquire, submitted by its own submit
        Acquire,            // acquire, then hold the slot for the later passes
        Held,               // the held slot, handed on
        ResendThenAcquire,  // the held slot sent first (the same column holds twice), then Acquire
    };

    /// An octagon acquires in `column`'s pass (a further column, 0, or none: the single render). `owner` is the column
    /// whose map the octagon is (0 player 1's, a seat's its index), or -1 for another painter.
    Take OnAcquire(int column, int owner) noexcept
    {
        if (column >= 1 && owner == column)
        {
            if (held_ && heldColumn_ == column)
            {
                return Take::ResendThenAcquire;  // held_ stays: the new slot is held in its place
            }
            if (held_)
            {
                return Take::Held;
            }
            held_       = true;
            heldColumn_ = column;
            return Take::Acquire;
        }
        if (column == 0 && held_ && owner == 0)
        {
            return Take::Held;
        }
        return Take::Game;
    }

    /// A submit in `column`'s pass, `ofHeld` if it is the held slot: true to skip it (still held for a later pass);
    /// false to send it, after which nothing is held.
    bool SkipSubmit(int column, bool ofHeld) noexcept
    {
        if (!ofHeld || !held_)
        {
            return false;
        }
        if (column >= 1)
        {
            return true;
        }
        held_       = false;
        heldColumn_ = -1;
        return false;
    }

    /// The frame's last render is done: true if a slot is still held (the caller sends it), and nothing is held after.
    bool EndFrame() noexcept
    {
        const bool stranded = held_;
        held_               = false;
        heldColumn_         = -1;
        return stranded;
    }

    bool Held() const noexcept { return held_; }

private:
    bool held_       = false;
    int  heldColumn_ = -1;
};

/// The entry name of a column's octagon: player 1's keeps the game's name; a further column's adds its number (column +
/// 1, so column 1's entry is number 2) to the FName's number part, a distinct name to the entry search without a
/// registration.
inline unsigned long long EntryName(unsigned long long name, int column) noexcept
{
    return column >= 1 ? name + (static_cast<unsigned long long>(column + 1) << 32) : name;
}

}  // namespace lss::hud::minimap_rules
