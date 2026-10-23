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

#include <algorithm>
#include <cstdint>

namespace lss::views::column_rules {

/// The most columns the mod lays out: player 1's and two further ones (hud/overlay_pass.h, players/scopes.h).
inline constexpr int kMaxColumns = 3;

/// The columns a frame shows. `playersIn`: player 1 and the seats in; `spanColumns`: the span's while held, else 0;
/// `override`: the console's, 0 for none. Never fewer than the players, never more than kMaxColumns, at least one.
constexpr int Wanted(int playersIn, int spanColumns, int override) noexcept
{
    const int wanted = override > 0 ? override : std::max(playersIn, spanColumns);
    return std::clamp(std::max(wanted, playersIn), 1, kMaxColumns);
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

/// What the layout module does with the entry it read, given whether three columns are wanted.
enum class Action
{
    Nothing,  // the entry is as it should be for the columns wanted
    Write,    // three columns wanted and the entry is the stock one: write the columns
    Restore,  // fewer wanted and the entry holds the mod's columns: put the stock records back
    Foreign,  // the entry is neither the stock one nor the mod's: left alone (logged once)
};

constexpr Action Decide(const Record (&entry)[kThreePlayerRecords], bool threeWanted) noexcept
{
    const bool stock = Same(entry, kStockFavorTop);
    const bool ours  = Same(entry, kThreeColumns);
    if (!stock && !ours)
    {
        return Action::Foreign;
    }
    if (threeWanted)
    {
        return stock ? Action::Write : Action::Nothing;
    }
    return ours ? Action::Restore : Action::Nothing;
}

}  // namespace lss::views::column_rules
