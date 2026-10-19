#pragma once

// Which deployed party member a further player plays (the user's rule, 2026-10-01, carried from Rebirth's
// player_two/party_member; RE-party-possession.md §3, RE-slot-member.md).
//
// Player 2 plays the member in the 2nd slot of the game's own party order: the row after player 1's leader in the
// deployed formation (PartyManager+0x230..+0x232, the order the party panel shows and the battle switch cycles),
// stepping past player 1's character as that cycle does; a third player would take the next row. The game's order
// is the only control: a menu reorder or a story party change moves the slot, and the session follows it. A member
// is taken only while nobody else's controller holds it; a downed member stays playable, as player 1 stays on a
// downed leader. With fewer rows than seats (a solo story party) that player has no member and waits.
//
// Seat-indexed (seat 1 = player 2); game thread only; every read guarded (a reload tears the party down under them).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::party_member {

bool Install(const platform::GameImage& image) noexcept;

enum class Wait : std::uint8_t
{
    None,     // `member` is the one to take
    NoSlot,   // the formation has no row for this seat beyond player 1's
    NotLoaded,  // the row names a type with no actor
    Held,     // another controller holds the row's member (its PLAYER slot)
    Unread,   // the party could not be read
};

struct Choice
{
    void*        member  = nullptr;
    std::uint8_t type    = 9;  // the row's player type (facts::kPlayerTypeNone with no row)
    std::int32_t ordinal = -1;         // its ordinal in the formation
    Wait         wait    = Wait::NoSlot;
};

/// The member seat `seat` plays now, or why it has none. `taker` is that seat's controller (a member it already
/// holds counts as free). Logged at Debug when `log` is set.
Choice Choose(int seat, void* playerOneController, void* taker, bool log) noexcept;

/// Why `member` is no longer seat `seat`'s to play: the slot names another type (a reorder), the member left the
/// deployed formation, or it became the game's leader. Null while it still is. `type` is the member's type as taken.
const wchar_t* WhyNotPlayable(int seat, void* member, std::uint8_t type, void* playerOneController) noexcept;

const wchar_t* Describe(Wait wait) noexcept;

}  // namespace lss::players::party_member
