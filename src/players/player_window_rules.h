#pragma once

// The pure rules of the player windows (players/player_window). Host-tested (tests/player_window_test.cpp).
//
// A pad is its player: slot N of the pad table is player N's. Each player has one window, on their own view:
//   - the d-pad left/right steps a further player to the next party member no other human plays;
//   - LB/RB moves the player's view one screen left/right, swapping places with what is there (another player's view or
//     a copy), so a person's own pad and their view end up in front of them;
//   - A, Start or a B tap closes it; B held a second takes player 2 or 3 out of the game (player 1 is refused).
// A pad on the title screen that presses a button becomes player 1's.

#include "players/pad_rules.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace lss::players::player_window_rules {

namespace pad = pad_rules;

inline constexpr int kSeats = 3;  // player 1 and two further players: the pad slots 0..2, the screens 0..2

inline constexpr std::uint16_t kDpadLeft  = 0x0004;
inline constexpr std::uint16_t kDpadRight = 0x0008;
inline constexpr std::uint16_t kLB        = 0x0100;  // XINPUT_GAMEPAD_LEFT_SHOULDER
inline constexpr std::uint16_t kRB        = 0x0200;  // XINPUT_GAMEPAD_RIGHT_SHOULDER

/// What a press in an open window asks for.
enum class Act : std::uint8_t
{
    Nothing,
    Step,     // the next party member, `dir` -1 left or +1 right (a further player's)
    Move,     // the view one screen `dir` -1 left or +1 right
    Close,
    Leave,    // B held: the player leaves the game
    Refused,  // B held on player 1's pad: player 1 never leaves
};

struct Action
{
    Act act = Act::Nothing;
    int dir = 0;

    constexpr bool operator==(const Action&) const noexcept = default;
};

/// One button's press (a down edge) on the pad of `seat`, whose window is open. B is not a press here: its tap and hold
/// come at its let-go or after a second (OnB).
constexpr Action OnPress(int seat, std::uint16_t button) noexcept
{
    switch (button)
    {
    case kDpadLeft: return seat >= 1 ? Action{Act::Step, -1} : Action{};
    case kDpadRight: return seat >= 1 ? Action{Act::Step, +1} : Action{};
    case kLB: return {Act::Move, -1};
    case kRB: return {Act::Move, +1};
    case pad::kA:
    case pad::kStart: return {Act::Close};
    default: return {};
    }
}

/// B on the pad of `seat` with its window open: a tap closes the window, a hold leaves (player 1: refused).
constexpr Action OnB(int seat, bool tap, bool held) noexcept
{
    if (held)
    {
        return {seat == 0 ? Act::Refused : Act::Leave};
    }
    return tap ? Action{Act::Close} : Action{};
}

/// Each screen's seat after `seat` moves one screen `dir` among `screens`: swapped with what is there (another player,
/// or -1 for a copy's screen). Empty at the row's edge, with one screen, or for a seat on no screen.
constexpr std::optional<std::array<int, kSeats>> Moved(std::array<int, kSeats> seatOn, int screens, int seat, int dir) noexcept
{
    screens = screens < 1 ? 1 : screens > kSeats ? kSeats : screens;
    for (int s = 0; s < screens; ++s)
    {
        if (seatOn[static_cast<std::size_t>(s)] != seat)
        {
            continue;
        }
        const int to = s + dir;
        if (to < 0 || to >= screens || dir == 0)
        {
            return std::nullopt;
        }
        seatOn[static_cast<std::size_t>(s)]  = seatOn[static_cast<std::size_t>(to)];
        seatOn[static_cast<std::size_t>(to)] = seat;
        return seatOn;
    }
    return std::nullopt;
}

/// The pad table with `user`'s pad on player 1's slot (swapped with the pad there).
constexpr pad::PadTable WithPlayerOne(pad::PadTable table, int user) noexcept
{
    const int from = pad::SlotOf(table, user);
    if (from > 0)
    {
        table[static_cast<std::size_t>(from)] = table[0];
        table[0]                              = user;
    }
    return table;
}

/// The pad table with `user`'s pad on `slot` (swapped with the pad there).
constexpr pad::PadTable WithPadOn(pad::PadTable table, int user, int slot) noexcept
{
    const int from = pad::SlotOf(table, user);
    if (from >= 0 && slot >= 0 && slot < pad::kSlots && from != slot)
    {
        table[static_cast<std::size_t>(from)] = table[static_cast<std::size_t>(slot)];
        table[static_cast<std::size_t>(slot)] = user;
    }
    return table;
}

/// The seat `user`'s pad plays (its slot, while a player holds it; slot 0 always), or -1 for a pad that plays no one.
constexpr int SeatOf(const pad::PadTable& table, std::uint32_t playing, int user) noexcept
{
    const int slot = pad::SlotOf(table, user);
    if (slot == 0)
    {
        return 0;
    }
    return slot > 0 && slot < kSeats && (playing & (1u << slot)) != 0 ? slot : -1;
}

/// The settings file's word for a column order ("213"), from its players left to right.
inline std::wstring OrderWord(const std::array<int, kSeats>& players)
{
    std::wstring word;
    for (const int player : players)
    {
        word += static_cast<wchar_t>(L'0' + player);
    }
    return word;
}

}  // namespace lss::players::player_window_rules
