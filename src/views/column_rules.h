#pragma once

// The pure rules of the view columns (plan/seats.md step 5: three columns). Host-tested (tests/column_rules_test.cpp).
//
// A column is a full-height view slot across the window: column 0 is player 1's, the seats' follow in order, and the
// mirror player fills the rest (views/mirror_player.h). How many columns the frame shows is one number for the whole
// mod: the players in, or the span's columns while it is held (one per monitor), or the console's override
// (`view-columns`, for the checks on one monitor).
//
// The engine lays three local players out from its own layout table's three-player entry (UGameViewportClient::
// SplitscreenInfo, ThreePlayer_FavorTop: one on top, two quadrants below; research/RE-layout-fov.md §2,
// RE-multi-monitor-views.md §7, RE-three-views.md §1). UE 4.18 has no three-column entry, so the mod writes that
// entry's three records to three equal columns while three columns are wanted and puts the stock records back after
// (views/column_layout.cpp). The records here are the ones compared and written.
//
// The engine gives the view at place i of its local-player list the entry's record i. The mod's columns are those
// places (player 1's, then the seats in join order, then the mirrors), and every per-view module counts in them. Where
// a column shows on screen is the column order's (the setting `column-order`, the user 2026-10-25): with `1 3 2`
// player 2's view takes the right-hand record and player 3's the middle one whatever the join order, through the same
// entry, the records arranged (Slots, Arrange). A view of no player (a mirror) takes a slot left over.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lss::views::column_rules {

/// The most columns the mod lays out: player 1's and two further ones (hud/overlay_pass.h, players/scopes.h).
inline constexpr int kMaxColumns = 3;

/// The columns a frame shows. `playersIn`: player 1 and the seats in; `spanColumns`: the span's while held, else 0;
/// `override`: the console's, 0 (or less) for none. Never fewer than the players and at least one, but never more
/// than kMaxColumns: with more players than columns built, the most wins (the seats are wired within the columns).
constexpr int Wanted(int playersIn, int spanColumns, int override) noexcept
{
    const int wanted = override > 0 ? override : std::max(playersIn, spanColumns);
    return std::clamp(std::max(wanted, playersIn), 1, kMaxColumns);
}

/// Whether three views are laid out as three columns (the mod's records) rather than the engine's own three-player
/// entry (player 1 across the top, two below: the user's pick for one window, 2026-10-24). Columns while the window
/// spans monitors (`spanColumns` the span's columns, 0 when out: on a row of three a column per monitor; a row of two
/// has no layout without a view across its seam and keeps the columns too) or the console asks for three
/// (`override`); in one window the engine's entry stays. Only when three views exist: `columns` wanted or
/// `localPlayers` the engine lists (a mirror kept a tick longer stays in a column).
constexpr bool ThreeColumns(int columns, int localPlayers, int spanColumns, int override) noexcept
{
    return (columns >= 3 || localPlayers >= 3) && (spanColumns > 0 || override >= 3);
}

/// One record of the engine's per-player layout data (FPerPlayerSplitscreenData): fractions of the viewport.
struct Record
{
    float sizeX = 0, sizeY = 0, originX = 0, originY = 0;
    bool  operator==(const Record&) const noexcept = default;
};

/// The engine's three-player layouts hold three records.
inline constexpr int kThreePlayerRecords = 3;

/// The stock ThreePlayer_FavorTop entry, as the viewport client's constructor fills it [CONFIRMED static,
/// RE-multi-monitor-views.md §7]: player 1 on top at full width, the other two in the quadrants below.
inline constexpr Record kStockFavorTop[kThreePlayerRecords] = {
    {1.0f, 0.5f, 0.0f, 0.0f},
    {0.5f, 0.5f, 0.0f, 0.5f},
    {0.5f, 0.5f, 0.5f, 0.5f},
};

/// Three equal full-height columns, left to right.
inline constexpr Record kThreeColumns[kThreePlayerRecords] = {
    {1.0f / 3.0f, 1.0f, 0.0f, 0.0f},
    {1.0f / 3.0f, 1.0f, 1.0f / 3.0f, 0.0f},
    {1.0f / 3.0f, 1.0f, 2.0f / 3.0f, 0.0f},
};

constexpr bool Same(const Record (&a)[kThreePlayerRecords], const Record (&b)[kThreePlayerRecords]) noexcept
{
    for (int i = 0; i < kThreePlayerRecords; ++i)
    {
        if (!(a[i] == b[i]))
        {
            return false;
        }
    }
    return true;
}

/// The column order: the player each screen slot shows, left to right (in one window, three players: the top, then
/// the bottom left, the bottom right). Player 1 keeps the first slot (the HUD tells player 1's view by its top-left
/// corner: hud/ui_targets.cpp); players 2 and 3 take the other two in either order.
struct Order
{
    std::array<int, kThreePlayerRecords> player{1, 2, 3};
    bool byNumber = false;  // `1 2 3` by player number, not the join order (the pad claim's: players/pad_claim)
    constexpr bool operator==(const Order&) const noexcept = default;
};

/// The default, `1 2 3`: the join order. The entry keeps the engine's list order (EntryWanted arranges nothing), so the
/// player who joined first shows in the middle (bottom left), the next on the right (bottom right), as before the
/// setting existed. `1 3 2` arranges by player number, and so does `1 2 3` with byNumber (the pad claim's choice,
/// whatever order the players joined in).
inline constexpr Order kJoinOrder{};

/// The order from text (the settings file, the console): its digits in order, anything else skipped ("1 3 2", "132",
/// "1,3,2"). Empty unless the digits are 1, then 2 and 3 in either order.
constexpr std::optional<Order> ParseOrder(std::wstring_view text) noexcept
{
    Order order{};
    int   count = 0;
    for (const wchar_t c : text)
    {
        if (c < L'0' || c > L'9')
        {
            continue;
        }
        if (count == kThreePlayerRecords)
        {
            return std::nullopt;
        }
        order.player[static_cast<std::size_t>(count++)] = c - L'0';
    }
    const bool valid = count == kThreePlayerRecords && order.player[0] == 1 &&
                       ((order.player[1] == 2 && order.player[2] == 3) || (order.player[1] == 3 && order.player[2] == 2));
    return valid ? std::optional<Order>{order} : std::nullopt;
}

/// The view at each place of the engine's list: the seat whose view it is (0 player 1's, 1 player 2's, 2 player 3's),
/// or -1 for a mirror's or no view.
using ListSeats = std::array<int, kThreePlayerRecords>;

/// The record slot each place's view takes: a player's view the slot the order gives that player, the others the
/// slots left over, in list order. Always a permutation of 0..2.
constexpr std::array<int, kThreePlayerRecords> Slots(const Order& order, const ListSeats& seats) noexcept
{
    std::array<int, kThreePlayerRecords>  slots{-1, -1, -1};
    std::array<bool, kThreePlayerRecords> taken{};
    for (int i = 0; i < kThreePlayerRecords; ++i)
    {
        for (int slot = 0; seats[static_cast<std::size_t>(i)] >= 0 && slot < kThreePlayerRecords; ++slot)
        {
            if (order.player[static_cast<std::size_t>(slot)] == seats[static_cast<std::size_t>(i)] + 1 &&
                !taken[static_cast<std::size_t>(slot)])
            {
                slots[static_cast<std::size_t>(i)] = slot;
                taken[static_cast<std::size_t>(slot)] = true;
            }
        }
    }
    for (int i = 0; i < kThreePlayerRecords; ++i)
    {
        for (int slot = 0; slots[static_cast<std::size_t>(i)] < 0 && slot < kThreePlayerRecords; ++slot)
        {
            if (!taken[static_cast<std::size_t>(slot)])
            {
                slots[static_cast<std::size_t>(i)] = slot;
                taken[static_cast<std::size_t>(slot)] = true;
            }
        }
    }
    return slots;
}

/// The entry for those slots: place i's record is base's record slots[i].
struct Records
{
    Record record[kThreePlayerRecords]{};
};
constexpr Records Arrange(const Record (&base)[kThreePlayerRecords], const std::array<int, kThreePlayerRecords>& slots) noexcept
{
    Records out{};
    for (int i = 0; i < kThreePlayerRecords; ++i)
    {
        out.record[i] = base[std::clamp(slots[static_cast<std::size_t>(i)], 0, kThreePlayerRecords - 1)];
    }
    return out;
}

/// The entry holds exactly `base`'s records, in any order (each once).
constexpr bool ArrangementOf(const Record (&entry)[kThreePlayerRecords], const Record (&base)[kThreePlayerRecords]) noexcept
{
    std::array<bool, kThreePlayerRecords> used{};
    for (int i = 0; i < kThreePlayerRecords; ++i)
    {
        bool found = false;
        for (int j = 0; !found && j < kThreePlayerRecords; ++j)
        {
            if (!used[static_cast<std::size_t>(j)] && entry[i] == base[j])
            {
                used[static_cast<std::size_t>(j)] = true;
                found                             = true;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}

/// What the module does with the entry it read, given the entry wanted (the stock one, or the mod's: the columns or
/// the stock records, arranged by the column order).
enum class Action
{
    Nothing,  // the entry is the one wanted
    Write,    // the mod's entry wanted, and the entry is the stock one or another of the mod's: write it
    Restore,  // the stock entry wanted, and the entry holds one of the mod's: put the stock records back
    Foreign,  // the entry is neither the stock one nor one of the mod's: left alone (logged once)
};

constexpr Action Decide(const Record (&entry)[kThreePlayerRecords], const Record (&wanted)[kThreePlayerRecords]) noexcept
{
    if (!ArrangementOf(entry, kStockFavorTop) && !ArrangementOf(entry, kThreeColumns))
    {
        return Action::Foreign;
    }
    if (Same(entry, wanted))
    {
        return Action::Nothing;
    }
    return Same(wanted, kStockFavorTop) ? Action::Restore : Action::Write;
}

/// The screen slot each place of the engine's list shows: arranged by an order other than the join order while three
/// views exist (`threeViews`); in the list's order otherwise (with two views the engine does not use the entry).
constexpr std::array<int, kThreePlayerRecords> ScreenSlots(bool threeViews, const Order& order, const ListSeats& seats) noexcept
{
    const bool arranged = threeViews && !(order == kJoinOrder);
    return arranged ? Slots(order, seats) : std::array<int, kThreePlayerRecords>{0, 1, 2};
}

/// The entry wanted: the mod's columns while three columns are wanted, else the stock records, its slots ScreenSlots'.
constexpr Records EntryWanted(bool threeColumns, bool threeViews, const Order& order, const ListSeats& seats) noexcept
{
    return Arrange(threeColumns ? kThreeColumns : kStockFavorTop, ScreenSlots(threeViews, order, seats));
}

/// The order that shows `seatOn` (each screen slot's seat, -1 for none): by player number, a slot no player has taking
/// the player left over. Empty unless player 1 is on the first slot and no seat is on two.
constexpr std::optional<Order> OrderShowing(const std::array<int, kThreePlayerRecords>& seatOn) noexcept
{
    Order                                 order{.byNumber = true};
    std::array<bool, kThreePlayerRecords> used{};
    for (int s = 0; s < kThreePlayerRecords; ++s)
    {
        const int seat = seatOn[static_cast<std::size_t>(s)];
        if (seat < -1 || seat >= kThreePlayerRecords || (seat >= 0 && used[static_cast<std::size_t>(seat)]))
        {
            return std::nullopt;
        }
        order.player[static_cast<std::size_t>(s)] = seat + 1;  // 0 for a slot no player has, filled below
        if (seat >= 0)
        {
            used[static_cast<std::size_t>(seat)] = true;
        }
    }
    for (int s = 0; s < kThreePlayerRecords; ++s)
    {
        for (int seat = 0; order.player[static_cast<std::size_t>(s)] == 0 && seat < kThreePlayerRecords; ++seat)
        {
            if (!used[static_cast<std::size_t>(seat)])
            {
                order.player[static_cast<std::size_t>(s)] = seat + 1;
                used[static_cast<std::size_t>(seat)]      = true;
            }
        }
    }
    if (order.player[0] != 1)
    {
        return std::nullopt;  // player 1's view keeps the first slot (the HUD's rule, above)
    }
    return order;
}

}  // namespace lss::views::column_rules
