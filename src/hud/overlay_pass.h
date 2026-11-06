#pragma once

// The HUD passes by column. A column is a view's place in the engine's list (where it shows on
// screen is the column order's, views/column_rules.h; each module reads a view's own rectangle): column 0 is player 1's,
// the game's own render of the overlay, run last in the frame; the further columns (1..) are the seats' (a seat's
// column is its place in the engine's list: the columns map, views/columns.h, set each tick) and get the mod's extra
// renders before it, highest first (hud/second_image.h runs them). The
// mirror's columns, and a seat's column on player 1's picture (listed as a mirror's: views/column_source.h HudMap),
// show column 0's image, or player 1's HUD laid out for a monitor of another shape (modules.cpp MirrorsOwnImage).
// Outside the passes, and during the game's one render while no
// further column has an image, the column is kSingle. Set by second_image, the one module that runs the passes; read by
// everything that paints into a menu-renderer slot during them (hud/overlay_layout.cpp, hud/minimap.cpp, the
// battle widgets). The rules the pass modules share are pure functions here (tests/overlay_pass_test.cpp). Game
// thread only.

#include "views/columns.h"

namespace lss::hud::overlay_pass {

/// Player 1's column and up to two further ones (plan/seats.md: three views).
inline constexpr int kMaxColumns = views::columns::kMaxColumns;
/// The game's one render, or no pass running.
inline constexpr int kSingle = -1;

/// The column whose pass runs now (kSingle outside the split's passes).
int  Column() noexcept;
void Set(int column) noexcept;
/// A pass of the split's render runs: column 0's or a further column's.
bool Split() noexcept;
/// A further column's pass runs: the mod's extra render.
bool Further() noexcept;

/// No seat: a mirror's column, an empty one, or a seat with no column.
inline constexpr int kNoSeat = -1;

/// The columns maps this tick (mod.cpp sets them before the HUD's tick and the frame's passes): `listed`, the engine's
/// list as columns (views/columns.h), and `hud`, its HUD form, where a seat's column on player 1's picture reads as a
/// mirror's (views/column_source.h HudMap). Game thread, as everything that asks.
void SetMap(const views::columns::Map& listed, const views::columns::Map& hud) noexcept;
/// The seat whose pass `column` is (0 player 1's, kNoSeat a mirror's or none), by the HUD's map.
int SeatOf(int column) noexcept;
/// The column the seat's HUD fills (-1 none, also while its column shows player 1's), by the HUD's map.
int ColumnOf(int seat) noexcept;
/// The column the seat's view is in, whatever it shows (-1 not listed), by the engine's list: for what follows the
/// seat's own local player, its sound listener.
int ListedColumnOf(int seat) noexcept;
/// The player whose HUD the pass of `column` shows: a seat's column its seat's; column 0, the single render and a
/// mirror's column with a pass of its own (hud/second_image.h, OtherShape) player 1's (0).
int ShownSeat(int column) noexcept;

/// Whether a view of `sizeX` x `sizeY` (fractions of the window) has another shape than player 1's `oneX` x `oneY`:
/// width over height more than 1 % apart. A mirror of another shape gets a HUD pass of its own: the game's composite
/// cover-fits player 1's image to a mirror's shape, so a wider mirror would show player 1's HUD enlarged and cut, a
/// narrower one inset. One of the same shape samples player 1's image, at no cost.
constexpr bool OtherShape(float sizeX, float sizeY, float oneX, float oneY) noexcept
{
    if (!(sizeX > 0.0f) || !(sizeY > 0.0f) || !(oneX > 0.0f) || !(oneY > 0.0f))
    {
        return false;
    }
    const float ratio = (sizeX / sizeY) / (oneX / oneY);  // the window's own shape cancels out
    return ratio > 1.01f || ratio < 1.0f / 1.01f;
}

/// The FName value of a column's layer entry: the stock name for column 0 (and the single render); for a further column
/// the stock name with another number, column + 1 (a distinct FName to the entry search's 8-byte compare, which needs
/// no registration; RE-hud-overlay-pass.md). Number is the FName's high dword.
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
