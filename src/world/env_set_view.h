#pragma once

// The pieces of world/env_sets that need no game: a further player's EnvironmentSet as the mod keeps it (one per
// seat), paced the way the game paces player 1's, and the edit of a player's hidden-actor list that removes only the
// mod's own entries (research/RE-picture-variants.md). Host-tested (tests/env_set_view_test.cpp).
//
// An entry is the game's own {FName actor, u32 Hidden} (12 bytes, the block's actor array).

#include <array>
#include <cstdint>
#include <span>

namespace lss::world::env_set_view {

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

/// A bounded list on static storage. The game's actor list keeps 32 entries inline and grows on the heap past that;
/// a set in Sector 5 lists more than 64 (seen live), so the bounds are wide.
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

inline constexpr int kMaxEntries = 2048;
inline constexpr int kMaxLevels  = 256;
inline constexpr int kMaxHidden  = 4096;  // actors hidden in one further player's view (listed, plus their attached actors)
using EntryList  = List<Entry, kMaxEntries>;
using LevelList  = List<Name, kMaxLevels>;
using HiddenList = List<std::uint8_t, kMaxLevels>;

/// A further player's set as the mod keeps it between the game's reads.
struct TwoSet
{
    EntryList  entries;  // the set's actor list
    LevelList  levels;   // the set's level list (where the game looks its actors up; world/area_levels loads it)
    HiddenList hidden;   // per level: 0 shown, 1 hidden, 2 hidden with its collision kept (parallel to levels)
    friend bool operator==(const TwoSet&, const TwoSet&) = default;
};

/// A further player's set after one read. Out (or switched off): nothing. A set read: that set. No set read (no ground
/// under that player that names one, or the trace not back yet): the last one kept, as the game keeps player 1's last
/// set when the ground under player 1 names none.
inline const TwoSet& NextTwoSet(bool present, bool resolved, const TwoSet& fresh, const TwoSet& last) noexcept
{
    static const TwoSet kNone{};
    return !present ? kNone : resolved ? fresh : last;
}

/// The game's pause after a change of player 1's set (`mgr+0x3C4`, 1 s): a new set is taken at once, then no other
/// until the pause has run out. At an area's edge the ground under a player can name two sets in turn every few
/// frames (seen live: 80 ms); the pause keeps the view from flipping with it.
inline constexpr double kChangePause = 1.0;

struct Paced
{
    TwoSet set;                // the set the player's view follows
    double changedAt = 0.0;    // when it was taken (seconds, any steady clock), once `changed`
    bool   changed   = false;  // a set has been taken (zero-based, so a record per seat costs the DLL nothing)
};

/// Takes `next` as the player's set if it differs and the pause since the last change has run out. True on a change.
/// `atOnce` (the player out, or the game's hold mode): no pause, so a set never outlives its player or its field
/// (world/area_levels loads what the paced set lists).
inline bool Pace(Paced& paced, const TwoSet& next, double now, bool atOnce = false) noexcept
{
    if (next == paced.set || (!atOnce && paced.changed && now - paced.changedAt < kChangePause))
    {
        return false;
    }
    paced.set       = next;
    paced.changedAt = now;
    paced.changed   = true;
    return true;
}

/// Removes from `items[0..num)` every pointer `ours` lists, keeping the others in order (other code's entries in an
/// engine list stay as they were). Returns the new count.
inline int RemoveOurs(void** items, int num, std::span<void* const> ours) noexcept
{
    int kept = 0;
    for (int i = 0; i < num; ++i)
    {
        bool mine = false;
        for (void* const our : ours)
        {
            mine = mine || items[i] == our;
        }
        if (!mine)
        {
            items[kept++] = items[i];
        }
    }
    return kept;
}

}  // namespace lss::world::env_set_view
