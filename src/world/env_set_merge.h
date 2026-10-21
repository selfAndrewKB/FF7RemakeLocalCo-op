#pragma once

// The pieces of world/env_sets_two that need no game: which of player 2's EnvironmentSet entries join player 1's, the
// levels the game's apply must search for them, and when the game must apply again (research/RE-picture-variants.md
// §5, §9). Host-tested (tests/env_set_merge_test.cpp).
//
// An entry is the game's own {FName actor, u32 Hidden} (12 bytes, the block's actor array). Player 1's list always
// comes first and is never changed; player 2's entries are added after it only for actors player 1's list does not
// name, so player 1's flag wins on a shared actor.

#include <array>
#include <cstdint>
#include <span>

namespace lss::world::env_set_merge {

/// An FName as the game stores it (comparison index, number).
struct Name
{
    std::uint32_t index  = 0;
    std::uint32_t number = 0;
    friend bool operator==(const Name&, const Name&) = default;
};

/// An entry of a set's actor list: the actor's name and whether the set hides it (1) or keeps it shown (0).
struct Entry
{
    Name          name;
    std::uint32_t hidden = 0;
    friend bool operator==(const Entry&, const Entry&) = default;
};
static_assert(sizeof(Name) == 8 && sizeof(Entry) == 12, "the game's layouts");

/// A bounded list on static storage (the game's own lists hold at most 32 actors; a set names a few levels).
template <typename T, int N>
struct List
{
    std::array<T, N> items{};
    int              count = 0;

    bool Add(const T& item) noexcept
    {
        if (count >= N)
        {
            return false;
        }
        items[static_cast<std::size_t>(count++)] = item;
        return true;
    }
    std::span<const T> View() const noexcept { return {items.data(), static_cast<std::size_t>(count)}; }
    friend bool operator==(const List& a, const List& b) noexcept
    {
        for (int i = 0; i < a.count && a.count == b.count; ++i)
        {
            if (!(a.items[static_cast<std::size_t>(i)] == b.items[static_cast<std::size_t>(i)]))
            {
                return false;
            }
        }
        return a.count == b.count;
    }
};

inline constexpr int kMaxEntries = 64;
inline constexpr int kMaxLevels  = 64;
using EntryList = List<Entry, kMaxEntries>;
using LevelList = List<Name, kMaxLevels>;

/// Player 2's entries for actors player 1's list does not name, in player 2's order. False if they did not all fit.
inline bool Extras(std::span<const Entry> one, std::span<const Entry> two, EntryList& out) noexcept
{
    out.count = 0;
    for (const Entry& entry : two)
    {
        bool named = false;
        for (const Entry& mine : one)
        {
            named = named || mine.name == entry.name;
        }
        for (const Entry& added : out.View())
        {
            named = named || added.name == entry.name;  // the game's own lists never repeat a name; keep it so
        }
        if (!named && !out.Add(entry))
        {
            return false;
        }
    }
    return true;
}

/// The levels in `two` that `one` does not list, appended to `out` (which may already hold some). False if full.
template <int N>
bool AddMissingLevels(std::span<const Name> one, std::span<const Name> two, List<Name, N>& out) noexcept
{
    for (const Name& level : two)
    {
        bool listed = false;
        for (const Name& mine : one)
        {
            listed = listed || mine == level;
        }
        for (const Name& added : out.View())
        {
            listed = listed || added == level;
        }
        if (!listed && !out.Add(level))
        {
            return false;
        }
    }
    return true;
}

/// Player 2's set as the mod keeps it between the game's reads.
struct TwoSet
{
    EntryList entries;  // the set's actor list
    LevelList levels;   // the set's level list (where the apply finds those actors)
};

/// Player 2's set after one read. Out (or switched off): nothing. A set read: that set. No set read (no ground under
/// player 2 that names one, or the trace not back yet): the last one kept, as the game keeps player 1's last set
/// when the ground under player 1 names none.
inline const TwoSet& NextTwoSet(bool present, bool resolved, const TwoSet& fresh, const TwoSet& last) noexcept
{
    static const TwoSet kNone{};
    return !present ? kNone : resolved ? fresh : last;
}

/// Whether the game must run its own change path again: the entries the mod adds now differ from the ones the game
/// last applied (player 2 entered or left a set, or left the session). Player 1's own changes are the game's to see.
inline bool MustApply(const EntryList& wanted, const EntryList& applied) noexcept
{
    return !(wanted == applied);
}

}  // namespace lss::world::env_set_merge
