#pragma once

// Where each column's view comes from, decided in one place each tick (plan/seats.md; cleanup step 5,
// plan/code-alignment.md A1: until it, `scene_camera`, `mirror_player` and mod.cpp's HUD tick each decided a
// part and nothing named the outcome per column). Pure rules over this tick's facts, host-tested
// (tests/column_source_test.cpp); the entry point (mod.cpp) applies the result and nothing else decides. The design
// is Rebirth's views/column_source (its seats step 6a, 2026-10-26), its names kept; the moments are Remake's.
//
// A column (views/columns.h) shows one of three things, one mechanism each:
//   - Self: the player's own view, their own camera and their own HUD pass (hud/second_image.h). A scene plays on
//     it through the scene camera copy (views/scene_camera.h: player 1's scene slots on the seat's own manager
//     while the scene holds the players, v0.40.0), and a shop the seat opens on a monitor row runs its own shop
//     camera there (world/shop_act.h): the row's look (the user's policy of 2026-10-21 and 2026-10-26: every view
//     plays the scene; the opener's view zooms on the shop);
//   - PlayerOne: the column stays and the mod puts player 1's camera and HUD in it, as a mirror player's column
//     shows them (views/mirror_player.h): every slot of player 1's camera manager is set on the seat's each frame
//     (scene_camera's every-slot copy), and the HUD passes count the column a mirror's (HudMap): it samples player
//     1's image, or, on a monitor of another shape, gets a pass of player 1's HUD laid out for it, as a mirror's does;
//   - Screen: no column at all; the whole screen is the engine's single view, player 1's (players/single_view.h
//     holds the viewport's split switch to it): the one-window look for the game's whole-screen moments (the user's
//     ruling of 2026-10-26: cutscenes, menus and minigames collapse as in Rebirth; the mirror look is the row's).
//
// The moments: the game shows something that takes the whole screen (views/whole_screen.h: a pause menu, a shop, a
// script menu, a pause-family widget, a cutscene, the event camera (a scripted talk, an event), a skippable scene, a
// cutscene skip, a minigame, a bike chase, a held black, the loading screen, the title; not a free talk or a pickup:
// the user, 2026-10-27) gives every further seat Screen in one window and PlayerOne while columns are held (the span,
// the console's columns): a held column is never black and never a split with a menu over it. A seat's own shop
// camera on its manager (a row's shop) is that seat's moment: every seat keeps Self through it (the opener's view zooms, the others play on; the shop holds the game for all), never
// the single view while a seat's own view is wanted; under the console's `on` the others go on player 1's picture
// (with player 1's HUD: HudMap). The mirror policy
// (the user, 2026-10-22: every column no player fills shows player 1's picture) is Why::NoPlayer; a handed seat (its
// member with the game's AI) keeps its own view, which the scene camera copy serves; the console's `single-view
// on|off` is the development override. The entry point applies the single view through the switch and, when the
// switch cannot be had, decides again with the columns held (Rebirth's review catch: never a split with a menu over
// it).
//
// The entry point logs each seat's change once, with its reason, in plain words (Describe). Kept for parity with
// Rebirth's, unused in Remake today: Shown's per-column sources, ViewsSplit and ListenerColumn (audio/listeners
// follows each seat's own controller), Map's mirrors.

#include "players/seat_state.h"
#include "players/seats.h"
#include "views/columns.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace lss::views::column_source {

/// Where a column's view comes from.
enum class Source : std::uint8_t
{
    Self,       // the player's own camera and HUD
    PlayerOne,  // player 1's camera and HUD in the player's column (the every-slot copy, no HUD pass), as a mirror's
    Screen,     // no column: the engine's single view, player 1's screen (one window only)
};

/// Why a column is not the player's own (None: it is).
enum class Why : std::uint8_t
{
    None,
    WholeScreen,  // the game takes the whole screen (views/whole_screen.h)
    Console,      // `single-view on`
    NoPlayer,     // no seat fills the column (a mirror player's view, a local player the mod does not know)
};

/// The console's override of the automatic choice (`single-view auto|on|off`).
enum class Override : std::uint8_t
{
    Auto,    // by the moments (the default)
    Single,  // always player 1's while a further player is in (development)
    Split,   // never (development)
};

/// This tick's facts, from the session, the columns and the whole-screen read.
struct Moment
{
    bool         columnsHeld = false;  // columns held beyond the players: the span's hold, or `view-columns`
    Override     choice      = Override::Auto;  // the console's `single-view`
    columns::Map map;                  // the engine's list as columns, after the mirror players' tick
    std::array<players::SeatState, players::scopes::kMaxPlayers> seats{};  // each seat's word (index 0, player 1's, unused)
    bool         wholeScreen = false;  // the game shows something that takes the whole screen (views/whole_screen.h)
    std::array<bool, players::scopes::kMaxPlayers> ownShop{};  // the seat's own shop camera runs on its manager (world/shop_act.h)
};

/// One column's source, with its reason.
struct SeatSource
{
    Source source = Source::Self;
    Why    why    = Why::None;

    bool operator==(const SeatSource&) const noexcept = default;
};

/// What the screen shows this tick.
struct Shown
{
    std::array<bool, players::scopes::kMaxPlayers> seatIn{};  // each seat in the game this tick (index 0 unused)
    /// By seat (index 0, player 1's, is always Self); a seat out of the game keeps Self with no reason.
    std::array<SeatSource, players::scopes::kMaxPlayers> seats{};
    /// By column (views/columns.h): the column's seat's source, player 1's for a column no seat fills.
    std::array<SeatSource, columns::kMaxColumns> columns{};
    int columnCount = 1;  // the columns listed

    /// The engine's single view wanted: some further seat in the game has Screen (every one has it then).
    constexpr bool Single() const noexcept
    {
        for (int seat = 1; seat < players::scopes::kMaxPlayers; ++seat)
        {
            const auto at = static_cast<std::size_t>(seat);
            if (seatIn[at] && seats[at].source == Source::Screen)
            {
                return true;
            }
        }
        return false;
    }

    /// A further player's own view is on screen: some further seat in the game shows their own. The split's
    /// per-player presentation (the HUD images, the per-view targets) runs then.
    constexpr bool SplitShown() const noexcept
    {
        for (int seat = 1; seat < players::scopes::kMaxPlayers; ++seat)
        {
            const auto at = static_cast<std::size_t>(seat);
            if (seatIn[at] && seats[at].source == Source::Self)
            {
                return true;
            }
        }
        return false;
    }

    /// More than one view on screen (two or three columns, not the engine's single view): the HUD is laid out for
    /// the columns then.
    constexpr bool ViewsSplit() const noexcept { return columnCount > 1 && !Single(); }

    /// Whether `seat`'s column shows player 1's camera and HUD by the mod (false for player 1, a seat out, and
    /// under the engine's single view, where the further column does not exist).
    constexpr bool OnPlayerOne(int seat) const noexcept
    {
        return players::SeatBuilt(seat) && seats[static_cast<std::size_t>(seat)].source == Source::PlayerOne;
    }

    /// The column a second sound listener would follow: the first further column listed whose source is not
    /// PlayerOne, or -1 when none is. Not applied in Remake (audio/listeners follows each seat's own controller);
    /// kept as Rebirth's.
    constexpr int ListenerColumn() const noexcept
    {
        for (int column = 1; column < columnCount; ++column)
        {
            if (columns[static_cast<std::size_t>(column)].source != Source::PlayerOne)
            {
                return column;
            }
        }
        return -1;
    }

    bool operator==(const Shown&) const noexcept = default;
};

/// The decision. Pure; every row of the plan's table is a test.
constexpr Shown Decide(const Moment& moment) noexcept
{
    Shown shown;
    shown.columnCount = moment.map.count > 0 ? moment.map.count : 1;
    if (shown.columnCount > columns::kMaxColumns)
    {
        shown.columnCount = columns::kMaxColumns;  // a map built by MapOf never is; the field is open
    }
    bool ownShopIn = false;  // some seat in the game runs its own shop camera: its view is wanted, no single view
    for (int seat = 1; seat < players::scopes::kMaxPlayers; ++seat)
    {
        const auto at = static_cast<std::size_t>(seat);
        shown.seatIn[at] = players::InGame(moment.seats[at]);
        ownShopIn        = ownShopIn || (shown.seatIn[at] && moment.ownShop[at]);
    }
    // Every further player's column, or the whole screen: the console's word, else the game's whole-screen moment
    // unless a seat's own shop camera runs (that seat's moment: every seat keeps its view). One window: the engine's
    // single view; columns held, or a seat's own shop camera on under the console's word: the columns stay and show
    // player 1's camera and HUD (a held column is never black; a seat's own view is never under the single view).
    const Why all = moment.choice == Override::Single                                             ? Why::Console
                    : moment.choice == Override::Auto && moment.wholeScreen && !ownShopIn ? Why::WholeScreen
                                                                                           : Why::None;
    const Source everyColumn = moment.columnsHeld || ownShopIn ? Source::PlayerOne : Source::Screen;
    for (int seat = 1; seat < players::scopes::kMaxPlayers; ++seat)
    {
        const auto at = static_cast<std::size_t>(seat);
        if (shown.seatIn[at] && all != Why::None && !moment.ownShop[at])
        {
            shown.seats[at] = {everyColumn, all};
        }
    }
    for (int column = 0; column < shown.columnCount; ++column)
    {
        const auto at   = static_cast<std::size_t>(column);
        const int  seat = moment.map.seatAt[at];
        if (column == 0 || seat == 0)  // player 1's column is their own, listed or not (an empty map)
        {
            shown.columns[at] = {Source::Self, Why::None};
        }
        else if (players::SeatBuilt(seat) && shown.seatIn[static_cast<std::size_t>(seat)])
        {
            shown.columns[at] = shown.seats[static_cast<std::size_t>(seat)];
        }
        else
        {
            shown.columns[at] = {Source::PlayerOne, Why::NoPlayer};
        }
    }
    return shown;
}

/// The map the HUD passes read (hud/overlay_pass.h SetMap): `map` with each further column whose source is PlayerOne
/// listed as a mirror's, since it shows what a mirror's shows, player 1's camera and HUD. Such a column on a monitor of
/// another shape than player 1's view then gets a pass of its own with player 1's HUD laid out for its monitor
/// (modules.cpp MirrorsOwnImage), where it sampled player 1's image before and the game's cover fit cut the image's top
/// and bottom off on a wider monitor (menus and subtitles). Pure.
constexpr columns::Map HudMap(const columns::Map& map, const Shown& shown) noexcept
{
    std::array<int, columns::kMaxColumns> places{};
    const int count = map.count < columns::kMaxColumns ? map.count : columns::kMaxColumns;
    for (int column = 0; column < count; ++column)
    {
        const auto at   = static_cast<std::size_t>(column);
        const int  seat = map.seatAt[at];
        places[at]      = column >= 1 && seat >= 1 && shown.columns[at].source == Source::PlayerOne ? columns::kMirror : seat;
    }
    return columns::MapOf(std::span<const int>(places.data(), static_cast<std::size_t>(count)));
}

/// For the log: `seat`'s source as it changed (empty while the same). The entry point logs a seat's change once,
/// for a seat in the game.
std::wstring Describe(int seat, const SeatSource& before, const SeatSource& after);

/// The reason's words.
const wchar_t* NameOf(Why why) noexcept;

/// The `single-view` command and reading (auto, on, off): the console's choice, applied at the next tick.
void RegisterCommands(Override& choice);

}  // namespace lss::views::column_source
