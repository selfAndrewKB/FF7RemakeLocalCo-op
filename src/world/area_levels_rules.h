#pragma once

// The pieces of world/area_levels that need no game: the one level request every player's area shares
// (research/RE-world-streaming-levers.md §1, §4.2-§4.4). Host-tested (tests/area_levels_rules_test.cpp).
//
// The game asks its slot-4 level request for player 1's area's level list, one byte per level: 0 shown, 1 hidden
// (collision off), 2 hidden (collision kept). The game loads what the list adds and unloads what it drops, so the list
// is what is loaded as well as what is shown. The merge adds each further player's area to it:
//   - player 1's levels first, in the game's order, then each further player's levels not listed yet;
//   - a level is listed once (a second copy takes a level reference the game never gives back, §4.3);
//   - its byte: shown if any player's area shows it, else hidden with its collision if any player's area keeps that,
//     else the first lister's (a player standing on a level must keep its floor; the per-view hides are separate);
//   - at most 256 levels (the request holds 256 and does not check, §4.2): all of player 1's, then the further
//     players' shown levels, then their hidden-with-collision, then the rest; whatever does not fit is cut and counted.

#include <array>
#include <cstdint>
#include <span>

namespace lss::world::area_levels_rules {

inline constexpr int kMaxLevels = 256;

/// One player's area as the game lists it: the level names (FName bits) and the parallel bytes (a missing byte reads 0).
struct Area
{
    std::span<const std::uint64_t> names;
    std::span<const std::uint8_t>  hidden;

    std::uint8_t HiddenAt(std::size_t i) const noexcept { return i < hidden.size() ? hidden[i] : 0; }
};

/// The request as the mod hands it to the game.
struct Request
{
    std::array<std::uint64_t, kMaxLevels> names{};
    std::array<std::uint8_t, kMaxLevels>  hidden{};
    int count   = 0;
    int added   = 0;  // the further players' levels player 1's area does not list
    int changed = 0;  // player 1's levels whose byte another player's area changed (shown, or its collision kept)
    int cut     = 0;  // levels that did not fit

    std::span<const std::uint64_t> Names() const noexcept { return {names.data(), static_cast<std::size_t>(count)}; }
    std::span<const std::uint8_t>  Hidden() const noexcept { return {hidden.data(), static_cast<std::size_t>(count)}; }

    friend bool operator==(const Request& a, const Request& b) noexcept
    {
        if (a.count != b.count)
        {
            return false;
        }
        for (int i = 0; i < a.count; ++i)
        {
            const auto at = static_cast<std::size_t>(i);
            if (a.names[at] != b.names[at] || a.hidden[at] != b.hidden[at])
            {
                return false;
            }
        }
        return true;
    }
};

/// A level's byte when two areas list it: shown wins, then hidden with collision kept, then the first lister's.
constexpr std::uint8_t Combine(std::uint8_t listed, std::uint8_t other) noexcept
{
    return listed == 0 || other == 0 ? 0 : listed == 2 || other == 2 ? 2 : listed;
}

/// The position of `name` in the request, or -1.
inline int Find(const Request& request, std::uint64_t name) noexcept
{
    for (int i = 0; i < request.count; ++i)
    {
        if (request.names[static_cast<std::size_t>(i)] == name)
        {
            return i;
        }
    }
    return -1;
}

/// The order the further players' levels are taken in when the request might fill: shown, hidden with collision, the rest.
constexpr int Rank(std::uint8_t hidden) noexcept
{
    return hidden == 0 ? 0 : hidden == 2 ? 1 : 2;
}

/// Player 1's area alone (`others` empty) or with the further players' areas added.
inline Request Merge(const Area& one, std::span<const Area> others) noexcept
{
    Request request;
    for (std::size_t i = 0; i < one.names.size(); ++i)
    {
        const int at = Find(request, one.names[i]);
        if (at >= 0)
        {
            request.hidden[static_cast<std::size_t>(at)] = Combine(request.hidden[static_cast<std::size_t>(at)], one.HiddenAt(i));
        }
        else if (request.count < kMaxLevels)
        {
            request.names[static_cast<std::size_t>(request.count)]  = one.names[i];
            request.hidden[static_cast<std::size_t>(request.count)] = one.HiddenAt(i);
            ++request.count;
        }
        else
        {
            ++request.cut;
        }
    }
    const int ones = request.count;
    for (int rank = 0; rank <= 2; ++rank)
    {
        for (const Area& area : others)
        {
            for (std::size_t i = 0; i < area.names.size(); ++i)
            {
                const std::uint8_t byte = area.HiddenAt(i);
                if (Rank(byte) != rank)
                {
                    continue;
                }
                const int at = Find(request, area.names[i]);
                if (at >= 0)
                {
                    std::uint8_t& held = request.hidden[static_cast<std::size_t>(at)];
                    const std::uint8_t next = Combine(held, byte);
                    request.changed += at < ones && next != held ? 1 : 0;
                    held = next;
                }
                else if (request.count < kMaxLevels)
                {
                    request.names[static_cast<std::size_t>(request.count)]  = area.names[i];
                    request.hidden[static_cast<std::size_t>(request.count)] = byte;
                    ++request.count;
                    ++request.added;
                }
                else
                {
                    ++request.cut;
                }
            }
        }
    }
    return request;
}

/// How many levels `area` lists hidden that the request shows: what that player's view shows and their single player
/// would not (the per-view hides' work, not built yet).
inline int ShownAgainst(const Request& request, const Area& area) noexcept
{
    int shown = 0;
    for (std::size_t i = 0; i < area.names.size(); ++i)
    {
        const int at = area.HiddenAt(i) != 0 ? Find(request, area.names[i]) : -1;
        shown += at >= 0 && request.hidden[static_cast<std::size_t>(at)] == 0 ? 1 : 0;
    }
    return shown;
}

}  // namespace lss::world::area_levels_rules
