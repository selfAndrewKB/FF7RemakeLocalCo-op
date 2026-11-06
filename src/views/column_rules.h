#pragma once

// The pure rules of the view columns (plan/seats.md step 5: three columns). Host-tested (tests/column_rules_test.cpp).
//
// A column is a view slot across the window, as tall as its monitor on a span: column 0 is player 1's, the seats' follow in order, and the
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
// a column shows on screen is the column order's (the setting `column-order`): by player number
// whatever the order the players joined in; with `1 3 2` player 2's view takes the right-hand record and player 3's the
// middle one, through the same entry, the records arranged (Slots, Arrange). A view of no player (a mirror) takes a slot left over. Any player may
// take any screen, player 1 too (nothing in the engine ties player 1 to the first record, RE-layout-fov.md
// §9, and the mod tells views by their place in the list, not on screen). Two views take one of the engine's two-player
// entries (top and bottom, or side by side), arranged the same way (TwoSlots): the two players in the order's order.

#include <algorithm>
#include <array>
#include <cmath>
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

/// Whether three views are laid out as the span's or the console's columns rather than the one-window layout (the setting
/// `three-player-split`). Columns while the window spans monitors (`spanColumns` the span's columns, 0 when out: on a
/// row of three a column per monitor; a row of two has no layout without a view across its seam and keeps the columns too)
/// or the console asks for three (`override`). Only when three views exist: `columns` wanted or `localPlayers` the engine
/// lists (a mirror kept a tick longer stays in a column).
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

/// Three equal full-height columns, left to right: the console's `view-columns 3` in one window.
inline constexpr Record kThreeColumns[kThreePlayerRecords] = {
    {1.0f / 3.0f, 1.0f, 0.0f, 0.0f},
    {1.0f / 3.0f, 1.0f, 1.0f / 3.0f, 0.0f},
    {1.0f / 3.0f, 1.0f, 2.0f / 3.0f, 0.0f},
};

/// The engine's two-player layouts hold two records.
inline constexpr int kTwoPlayerRecords = 2;

/// The stock two-player entries, as the constructor fills them [CONFIRMED static, RE-layout-fov.md §9.3]: TwoPlayer_
/// Horizontal (player 1 on top, the other below) and TwoPlayer_Vertical (player 1 on the left, the other on the right).
inline constexpr Record kStockTopBottom[kTwoPlayerRecords] = {
    {1.0f, 0.5f, 0.0f, 0.0f},
    {1.0f, 0.5f, 0.0f, 0.5f},
};
inline constexpr Record kStockSideBySide[kTwoPlayerRecords] = {
    {0.5f, 1.0f, 0.0f, 0.0f},
    {0.5f, 1.0f, 0.5f, 0.0f},
};

template <std::size_t N>
constexpr bool Same(const Record (&a)[N], const Record (&b)[N]) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!(a[i] == b[i]))
        {
            return false;
        }
    }
    return true;
}

/// The column order: the player each screen slot shows, left to right (in one window, three players: the large view, then
/// the other two, top or left first; two players: the top, then the bottom, or left and right). Any of the six orders;
/// with two players the one not in is skipped. Always by player number, never by the order the players joined in.
struct Order
{
    std::array<int, kThreePlayerRecords> player{1, 2, 3};
    constexpr bool operator==(const Order&) const noexcept = default;
};

/// The default, `1 2 3`: players 1, 2 and 3 left to right (in one window, three players: player 1 on top, player 2 bottom
/// left, player 3 bottom right). With the players listed in that order the entry is the stock one, untouched.
inline constexpr Order kDefaultOrder{};

/// The order from text (the settings file, the console): its digits in order, anything else skipped ("1 3 2", "132",
/// "1,3,2"). Empty unless the digits are 1, 2 and 3, each once.
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
    std::array<bool, kThreePlayerRecords + 1> seen{};
    for (int i = 0; i < count; ++i)
    {
        const int player = order.player[static_cast<std::size_t>(i)];
        if (player < 1 || player > kThreePlayerRecords || seen[static_cast<std::size_t>(player)])
        {
            return std::nullopt;
        }
        seen[static_cast<std::size_t>(player)] = true;
    }
    return count == kThreePlayerRecords ? std::optional<Order>{order} : std::nullopt;
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

/// The record each of two views takes (the engine's two-player entries): the two players in the order's order (the
/// player not in skipped); the list's order when a view is no player's (a mirror).
constexpr std::array<int, kTwoPlayerRecords> TwoSlots(const Order& order, const ListSeats& seats) noexcept
{
    const auto rank = [&order](int seat) {
        for (int slot = 0; slot < kThreePlayerRecords; ++slot)
        {
            if (order.player[static_cast<std::size_t>(slot)] == seat + 1)
            {
                return slot;
            }
        }
        return kThreePlayerRecords;
    };
    const int first = seats[0], second = seats[1];
    if (first < 0 || second < 0 || rank(first) <= rank(second))
    {
        return {0, 1};
    }
    return {1, 0};
}

/// An entry's records: place i's record is record[i].
template <std::size_t N>
struct RecordSet
{
    Record record[N]{};
};
using Records    = RecordSet<kThreePlayerRecords>;
using TwoRecords = RecordSet<kTwoPlayerRecords>;

/// How three players share one window (the setting `three-player-split`). A span of monitors is always columns.
enum class ThreeSplit : std::uint8_t
{
    Top,      // the large view across the top, two below: the engine's own entry
    Bottom,   // two above, the large view across the bottom
    Left,     // the large view down the left, two stacked on the right
    Right,    // two stacked on the left, the large view down the right
};

constexpr std::optional<ThreeSplit> ParseThreeSplit(std::wstring_view word) noexcept
{
    return word == L"top"      ? std::optional<ThreeSplit>{ThreeSplit::Top}
           : word == L"bottom" ? std::optional<ThreeSplit>{ThreeSplit::Bottom}
           : word == L"left"   ? std::optional<ThreeSplit>{ThreeSplit::Left}
           : word == L"right"  ? std::optional<ThreeSplit>{ThreeSplit::Right}
                               : std::nullopt;
}

constexpr const wchar_t* ThreeSplitWord(ThreeSplit split) noexcept
{
    switch (split)
    {
        case ThreeSplit::Bottom: return L"bottom";
        case ThreeSplit::Left:   return L"left";
        case ThreeSplit::Right:  return L"right";
        case ThreeSplit::Top:
        default:                 return L"top";
    }
}

/// The records in screen order, the large view first and the other two top or left first, so the screen order means the
/// same in each. Written into the one entry the engine reads (the three-player setting held at FavorTop).
constexpr Records ThreeSplitRecords(ThreeSplit split) noexcept
{
    switch (split)
    {
        case ThreeSplit::Bottom:
            return {{{1.0f, 0.5f, 0.0f, 0.5f}, {0.5f, 0.5f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.0f}}};
        case ThreeSplit::Left:
            return {{{0.5f, 1.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.5f}}};
        case ThreeSplit::Right:
            return {{{0.5f, 1.0f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.0f, 0.5f}}};
        case ThreeSplit::Top:
        default:
            return {{kStockFavorTop[0], kStockFavorTop[1], kStockFavorTop[2]}};
    }
}

/// Whether two views sit side by side: always on a span of monitors (`spanColumns` above 0, one view per monitor), else the
/// setting `two-player-split`.
constexpr bool TwoSideBySide(int spanColumns, bool settingSideBySide) noexcept
{
    return spanColumns > 0 || settingSideBySide;
}

/// `pixels` of `total` as the fraction the engine turns back into exactly `pixels`. The engine places a view at
/// TruncToInt(fraction * viewport size), the product in float (ULocalPlayer::GetProjectionData, stock 4.18 [CONFIRMED
/// src]), and the nearest float to 2560/8560 lands a hair below 2560: the view would start a pixel short of its monitor's
/// edge. So the fraction is stepped up to the first float whose product truncates to `pixels` (0 for bad input).
inline float EdgeFraction(std::int32_t pixels, std::int32_t total) noexcept
{
    if (pixels <= 0 || total <= 0 || pixels > total)
    {
        return 0.0f;
    }
    const float size     = static_cast<float>(total);
    float       fraction = static_cast<float>(pixels) / size;
    for (int step = 0; step < 4 && std::trunc(fraction * size) < static_cast<float>(pixels); ++step)
    {
        fraction = std::nextafter(fraction, 2.0f);
    }
    return fraction;
}

/// Full-height columns for a row of monitors of these widths, left to right (the span's, views/monitor_row): each view
/// fills its own monitor edge to edge, a wider monitor a wider view, as the game fits a single view to the width of the
/// monitor it is on. Equal widths give equal columns. Any width not above 0: every record empty.
template <std::size_t N>
RecordSet<N> ColumnRecords(const std::array<std::int32_t, N>& widths) noexcept
{
    RecordSet<N> out{};
    std::int32_t total = 0;
    for (const std::int32_t width : widths)
    {
        if (width <= 0)
        {
            return out;
        }
        total += width;
    }
    std::int32_t left = 0;
    for (std::size_t i = 0; i < N; ++i)
    {
        out.record[i] = {EdgeFraction(widths[i], total), 1.0f, left == 0 ? 0.0f : EdgeFraction(left, total), 0.0f};
        left += widths[i];
    }
    return out;
}

/// The same with each view as tall as its monitor, its top at the window's top: a monitor shorter than the row's tallest shows
/// the top part of the window, where its view is.
template <std::size_t N>
RecordSet<N> ColumnRecords(const std::array<std::int32_t, N>& widths, const std::array<std::int32_t, N>& heights) noexcept
{
    RecordSet<N> out   = ColumnRecords(widths);
    std::int32_t tallest = 0;
    for (const std::int32_t height : heights)
    {
        tallest = std::max(tallest, height);
    }
    for (std::size_t i = 0; i < N; ++i)
    {
        if (heights[i] <= 0)
        {
            return ColumnRecords(widths);
        }
        out.record[i].sizeY = heights[i] == tallest ? 1.0f : EdgeFraction(heights[i], tallest);
    }
    return out;
}

/// The entry for those slots: place i's record is base's record slots[i].
template <std::size_t N>
constexpr RecordSet<N> Arrange(const Record (&base)[N], const std::array<int, N>& slots) noexcept
{
    RecordSet<N> out{};
    for (std::size_t i = 0; i < N; ++i)
    {
        out.record[i] = base[static_cast<std::size_t>(std::clamp(slots[i], 0, static_cast<int>(N) - 1))];
    }
    return out;
}

/// The entry holds exactly `base`'s records, in any order (each once).
template <std::size_t N>
constexpr bool ArrangementOf(const Record (&entry)[N], const Record (&base)[N]) noexcept
{
    std::array<bool, N> used{};
    for (std::size_t i = 0; i < N; ++i)
    {
        bool found = false;
        for (std::size_t j = 0; !found && j < N; ++j)
        {
            if (!used[j] && entry[i] == base[j])
            {
                used[j] = true;
                found   = true;
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
/// the one-window layout, arranged by the column order).
enum class Action
{
    Nothing,  // the entry is the one wanted
    Write,    // the mod's entry wanted, and the entry is the stock one or another of the mod's: write it
    Restore,  // the stock entry wanted, and the entry holds one of the mod's: put the stock records back
    Foreign,  // the entry is neither the stock one nor one of the mod's: left alone (logged once)
};

/// `columns` the columns in effect (the span's monitors' widths, or equal thirds); `written` the entry the mod wrote
/// last (the stock one before any write): the columns follow the span's monitors, so the entry may still hold the
/// records of an earlier row.
constexpr Action Decide(const Record (&entry)[kThreePlayerRecords], const Record (&wanted)[kThreePlayerRecords],
                        const Record (&columns)[kThreePlayerRecords], const Record (&written)[kThreePlayerRecords]) noexcept
{
    if (!ArrangementOf(entry, kStockFavorTop) && !ArrangementOf(entry, columns) && !ArrangementOf(entry, kThreeColumns) &&
        !Same(entry, written))
    {
        return Action::Foreign;
    }
    if (Same(entry, wanted))
    {
        return Action::Nothing;
    }
    return Same(wanted, kStockFavorTop) ? Action::Restore : Action::Write;
}

/// The screen slot each place of the engine's list shows with `views` views: three by the order (Slots), two by the
/// order's order of the two players (TwoSlots, the third place unused); one in place.
constexpr std::array<int, kThreePlayerRecords> ScreenSlots(int views, const Order& order, const ListSeats& seats) noexcept
{
    if (views >= kThreePlayerRecords)
    {
        return Slots(order, seats);
    }
    if (views == kTwoPlayerRecords)
    {
        const auto two = TwoSlots(order, seats);
        return {two[0], two[1], 2};
    }
    return {0, 1, 2};
}

/// The three-player entry wanted: the mod's columns (`columns`: the span's widths, or equal thirds) while three columns
/// are wanted, else the one-window layout (`oneWindow`: ThreeSplitRecords), arranged while three views exist; the stock
/// records while they do not (nothing reads the entry then).
constexpr Records EntryWanted(bool threeColumns, const Record (&columns)[kThreePlayerRecords],
                              const Record (&oneWindow)[kThreePlayerRecords], bool threeViews, const Order& order,
                              const ListSeats& seats) noexcept
{
    if (!threeViews)
    {
        return Arrange(kStockFavorTop, ScreenSlots(1, order, seats));
    }
    return Arrange(threeColumns ? columns : oneWindow, ScreenSlots(kThreePlayerRecords, order, seats));
}

/// A two-player entry wanted (`base` its records in screen order: the constructor's, or for side by side on a span the
/// two monitors' widths): arranged while exactly two views exist.
constexpr TwoRecords TwoEntryWanted(const Record (&base)[kTwoPlayerRecords], bool twoViews, const Order& order,
                                    const ListSeats& seats) noexcept
{
    return Arrange(base, twoViews ? TwoSlots(order, seats) : std::array<int, kTwoPlayerRecords>{0, 1});
}

/// What to do with a two-player entry: as Decide, with its own stock records, the base in effect (`base`) and the entry
/// last written (`written`) as the known contents.
constexpr Action DecideTwo(const Record (&entry)[kTwoPlayerRecords], const Record (&wanted)[kTwoPlayerRecords],
                           const Record (&stock)[kTwoPlayerRecords], const Record (&base)[kTwoPlayerRecords],
                           const Record (&written)[kTwoPlayerRecords]) noexcept
{
    if (!ArrangementOf(entry, stock) && !ArrangementOf(entry, base) && !Same(entry, written))
    {
        return Action::Foreign;
    }
    if (Same(entry, wanted))
    {
        return Action::Nothing;
    }
    return Same(wanted, stock) ? Action::Restore : Action::Write;
}

/// The order that shows `seatOn` (each screen slot's seat, -1 for none): by player number, a slot no player has taking
/// the player left over. Empty when a seat is on two slots or is no seat.
constexpr std::optional<Order> OrderShowing(const std::array<int, kThreePlayerRecords>& seatOn) noexcept
{
    Order                                 order{};
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
    return order;
}

}  // namespace lss::views::column_rules
