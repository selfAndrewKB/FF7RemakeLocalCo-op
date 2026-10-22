#pragma once

// The HUD passes by column (plan/seats.md step 4). A column is a view slot across the window: column 0 is player 1's,
// the game's own render of the overlay, run last in the frame; the further columns (1..) are the seats' (a seat's
// column is its index) and get the mod's extra renders before it, highest first (hud/second_image.h runs them). The
// mirror's columns show column 0's image and get no pass. Outside the passes, and during the game's one render while no
// further column has an image, the column is kSingle. Set by second_image, the one module that runs the passes; read by
// everything that paints into a menu-renderer slot during them (hud/overlay_layout.cpp, hud/minimap_two.cpp, the
// battle widgets). The rules the pass modules share are pure functions here (tests/overlay_pass_test.cpp). Game
// thread only.

namespace lss::hud::overlay_pass {

/// Player 1's column and up to two further ones (plan/seats.md: three views).
inline constexpr int kMaxColumns = 3;
/// The game's one render, or no pass running.
inline constexpr int kSingle = -1;

/// The column whose pass runs now (kSingle outside the split's passes).
int  Column() noexcept;
void Set(int column) noexcept;
/// A pass of the split's render runs: column 0's or a further column's.
bool Split() noexcept;
/// A further column's pass runs: the mod's extra render.
bool Further() noexcept;

/// The FName value of a column's layer entry: the stock name for column 0 (and the single render); for a further column
/// the stock name with another number, column + 1 (a distinct FName to the entry search's 8-byte compare, which needs
/// no registration; RE-hud-overlay-pass.md §3.3). Number is the FName's high dword.
constexpr unsigned long long EntryName(unsigned long long stockName, int column) noexcept
{
    return column >= 1 ? stockName + (static_cast<unsigned long long>(column + 1) << 32) : stockName;
}

/// What a pass does at a ring slot's acquire, for a pool whose ring may turn only once a frame (pool 2's overlay slot,
/// pool 0's navi map slot): the frame's first pass acquires and holds the slot, every later pass takes the held one,
/// and column 0's pass, the last, consumes it (its submit sends the slot holding every column's entry).
enum class SlotAction
{
    Acquire,         // the game's own acquire (a single render, or no slot held and nothing to hold for)
    AcquireAndHold,  // a further pass with no slot held yet
    TakeHeld,        // a further pass after it: the same slot, still held
    ConsumeHeld,     // column 0's pass: the held slot, forgotten
};

constexpr SlotAction AcquireAction(int column, bool held) noexcept
{
    if (column < 0)
    {
        return SlotAction::Acquire;
    }
    if (held)
    {
        return column == 0 ? SlotAction::ConsumeHeld : SlotAction::TakeHeld;
    }
    return column >= 1 ? SlotAction::AcquireAndHold : SlotAction::Acquire;
}

/// The render thread's draw gate (hud/image_entries.h): whether the view of column `view` skips the layer entry owned
/// by column `owner` (-1: an entry of no column, never skipped; `view` -1: no split view identity, nothing skipped).
/// Each view draws its own column's entry; column 0's is drawn in a further view only while the slot holds no entry of
/// a further column (`furtherSeen` false: a frame from before the image started).
constexpr bool DrawSkipped(int view, int owner, bool furtherSeen) noexcept
{
    if (view < 0 || owner < 0 || owner == view)
    {
        return false;
    }
    return owner != 0 || furtherSeen;
}

}  // namespace lss::hud::overlay_pass
