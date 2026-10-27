#pragma once

// The module table: every module the entry point installs after the session, in install order, each with its
// removal or none and the rows it needs (modules.cpp). The entry point (mod.cpp) installs the table in order and removes
// it in reverse; each module logs its own install line, one whose check fails stands aside alone, and a row whose
// needed row stood aside stands aside too (said once). The order constraints the modules carry (the shadows' call
// site before the area blend, every needed row before the row that needs it) are pinned by static_asserts beside
// the table: the build fails if a row moves past one. The design is Rebirth's modules.h (its cleanup of 2026-10;
// cleanup step 6, plan/code-alignment.md A2: until it mod.cpp kept the install, remove and tick orders as three hand-
// kept lists with nested install conditions); changed for Remake: a row names the rows it needs instead of each
// module checking its dependency at install, and the world hooks the seats' members use are removed only with every
// further player out, as before (RemoveWithPlayersOut).
//
// Outside the table, in the entry point: what every module relies on (the weak pointers, the field root, the session
// and the modules it installs itself) before it; the session's commands, the console and the engine callback after
// it. The mirror views are cleared and the split's show-flag mask put back by the entry point before the table's
// removal, as before.
//
// The same file lists the modules' per-tick calls, in three phases around the session's tick: TickBeforeSession (the
// game's state and the pads, read before the session and the console act), TickColumns (the letterbox, the window
// span, the column layout and the mirror views toward the columns the frame shows) and TickAfterSession(Frame) (the
// render, scene camera, battle, world and HUD modules over the frame's facts). The entry point keeps the keys, the
// console, the session's tick, the columns map and the columns' sources (views/column_source.h) between them, and
// fills the Frame once.

#include "players/seat_state.h"
#include "players/seats.h"
#include "views/column_rules.h"
#include "views/column_source.h"
#include "views/columns.h"

#include <array>
#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::modules {

/// Installs every module in table order; a row whose needed row is not installed stands aside (logged).
void Install(const platform::GameImage& image) noexcept;

/// Removes every row removed whatever the players, in reverse table order (before the session's Remove). A row
/// without a removal keeps its patches in place, pointing into the pinned DLL, as the entry point left them before.
void Remove() noexcept;

/// Removes the rows removed only with every further player out (the world hooks the seats' members use: the field
/// targets and their consumers, the traversals), in reverse table order; the entry point calls it after the
/// session's Remove when no further player is in.
void RemoveWithPlayersOut() noexcept;

/// The mirror player row installed (the entry point then hands the session the mirrors' hooks).
bool MirrorPlayerInstalled() noexcept;

/// Game thread, once per engine tick, after the console's lines and before the session's tick: the seats' input
/// tick kept, the game's state read (the session reads this tick's values; the console's lines, before it, the last
/// tick's), the whole-screen read over it (the column source's moment), the pads.
void TickBeforeSession() noexcept;

/// What the columns' tick reads from the session.
struct ColumnsIn
{
    int                       playersIn = 1;  // player 1 and the seats in: the columns the players fill
    std::uint32_t             seatIds   = 0;  // the seats' controller ids (the mirror views skip them)
    views::column_rules::ListSeats listSeats{0, -1, -1};  // the engine's list as the column order takes it, before this tick's mirrors
};

/// The columns of one tick (views/column_rules.h).
struct Columns
{
    int  held        = 0;      // the span's columns while held, else 0
    int  wanted      = 1;      // the columns the frame shows: the players', the span's while held, or the console's
    int  alone       = 1;      // the columns with player 1 alone (the mirrors' count while no seat is in)
    bool columnsHeld = false;  // columns held beyond the players: the span's hold or the console's view-columns
};

/// Game thread, once per engine tick right after the session's: the letterbox patch and the window span toward the
/// players in, the columns wanted, the engine's three-player layout and the mirror views toward them, the split
/// scale. The entry point reads the engine's list into the columns map after it.
Columns TickColumns(const ColumnsIn& in) noexcept;

/// What the modules read once per engine tick, after the session's tick and the columns' sources and before any
/// module of the phase ticks: a battle tick that released a member would leave the world modules the released
/// member for this tick (none does today: members are bound and released through the session alone); the entry
/// point fills it from the session.
struct Frame
{
    struct Seat
    {
        players::SeatState state       = players::SeatState::Out;  // the session's word for the seat
        bool               seated      = false;    // the seat's player holds its member
        void*              localPlayer = nullptr;  // the seat's local player while it holds one
        void*              controller  = nullptr;  // its controller
        void*              manager     = nullptr;  // its camera manager
        void*              member      = nullptr;  // its member while the seat is in the game

        /// The session holds the seat's player (in, playing or handed).
        constexpr bool In() const noexcept { return players::InGame(state); }
    };
    std::array<Seat, players::scopes::kMaxPlayers> seats{};  // by seat (players/seats.h); index 0, player 1's, unused
    int   count      = 0;        // the seats in
    void* managerOne = nullptr;  // player 1's camera manager
    void* pawnOne    = nullptr;  // player 1's pawn (the character the NPCs' senses centre on)
    views::columns::Map         map;    // the engine's list as columns, after this tick's mirrors (views/columns.h)
    views::column_source::Shown shown;  // what each column shows this tick (views/column_source.h)
    std::array<void*, views::columns::kMaxColumns> localPlayer{};  // by column: the engine list's local player
    std::array<void*, views::columns::kMaxColumns> controller{};   // by column: its controller
};

/// Game thread, once per engine tick, after the session's tick and the columns' sources: the split's render and
/// area blends, the scene camera, the battle modules, the world around the views and the HUD laid out for the
/// columns, each over the frame's facts.
void TickAfterSession(const Frame& frame) noexcept;

}  // namespace lss::modules
