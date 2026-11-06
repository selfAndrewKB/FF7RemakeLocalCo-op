#pragma once

// A seat's state, derived from the session's record of it (out, awaiting, rejoining, in, playing,
// handed). The session keeps flags; the readings and
// the log want one word. Pure, host-tested (tests/seat_state_test.cpp).

namespace lss::players {

enum class SeatState
{
    Out,        // no player in the seat
    Awaiting,   // asked for; joins once its member is free
    Rejoining,  // taken out by a world change; joins again once the field is back
    In,         // the player is in, between a seat and the next (momentary)
    Playing,    // holds its member
    Handed,     // its member is with its AI while the game's leader is, or for the member's own hold; the seat is kept
};

/// The session's flags for one seat, as its record keeps them.
struct SeatFlags
{
    bool in            = false;  // its local player is listed by the engine
    bool seated        = false;  // it holds its member
    bool handedToAi    = false;  // the member is with its AI (the hand)
    bool awaiting      = false;  // asked for, out until the row's member is free
    bool rejoinPending = false;  // taken out by a world change, joins again when the field is back
};

/// One word for the flags. Out of the game: a wait for a member first (a waiting player keeps waiting through a
/// world change, which marks it for a rejoin too), then a pending rejoin (the world change took a playing player
/// out and brings them back), else out. In the game: without a member, "in"; with it, handed or playing.
constexpr SeatState StateOf(const SeatFlags& f) noexcept
{
    if (!f.in)
    {
        return f.awaiting ? SeatState::Awaiting : f.rejoinPending ? SeatState::Rejoining : SeatState::Out;
    }
    if (!f.seated)
    {
        return SeatState::In;
    }
    return f.handedToAi ? SeatState::Handed : SeatState::Playing;
}

/// The seat's player is in the game (its local player listed): in, playing or handed.
constexpr bool InGame(SeatState state) noexcept
{
    return state == SeatState::In || state == SeatState::Playing || state == SeatState::Handed;
}

/// The reading's word.
constexpr const wchar_t* StateName(SeatState state) noexcept
{
    switch (state)
    {
    case SeatState::Out: return L"out";
    case SeatState::Awaiting: return L"awaiting";
    case SeatState::Rejoining: return L"rejoining";
    case SeatState::In: return L"in";
    case SeatState::Playing: return L"playing";
    case SeatState::Handed: return L"handed";
    }
    return L"?";
}

}  // namespace lss::players
