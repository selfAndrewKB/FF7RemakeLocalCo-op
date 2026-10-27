#pragma once

// A member's own chain of level states ahead of its current setting, walked as the game's data lays it out.
//
// A level state row names the member's AI setting (its Status: a run "MoveToTarget", a "Lead", a "Stay", a "Stable"
// stand, ...) and the states it moves on to (NextStateChange, one per trigger). A hold on a member blocks only the
// member's own arrival signal, so it forecloses every state beyond the current one; the walk lists them. Breadth
// first from the state the arrival leads to, each state once, a Stable ending its branch (the member stands again:
// what follows is a later beat's script), at most `maxLinks` states. The caller supplies the row lookup (the game's
// state-change tables in the mod, a map in the tests) and reads what the walk found: the first further move (the lead
// gate's test, players/lead_gate.h) and every move with its destination (where the path goes).
//
// Pure (host-tested in tests/state_path_test.cpp).

#include "players/field_hold.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lss::players::state_path {

/// A state row as the walk needs it.
struct Row
{
    std::wstring              status;           // the AI setting's name
    std::vector<std::wstring> next;             // the states it moves on to, in trigger order
    std::int64_t              destination = 0;  // a move's destination name (the game's FName bits; 0: none)
};

/// One state reached.
struct Link
{
    std::wstring name;
    std::wstring status;             // empty while `read` is false
    std::int64_t destination = 0;
    std::size_t  parent      = 0;    // the link it was reached from (the first is its own parent)
    bool         read        = false;
};

struct Walk
{
    std::vector<Link>          links;
    std::optional<std::size_t> firstMove;          // the first further move reached, breadth first
    bool                       truncated = false;  // a state beyond the limit was left unwalked
};

/// Walks from `first`; `lookup(name)` answers std::optional<Row> (nullopt: the row is unknown).
template <typename Lookup>
Walk WalkFrom(const std::wstring& first, Lookup&& lookup, std::size_t maxLinks)
{
    Walk walk;
    if (first.empty() || maxLinks == 0)
    {
        return walk;
    }
    walk.links.push_back(Link{.name = first});
    for (std::size_t i = 0; i < walk.links.size(); ++i)
    {
        std::optional<Row> row = lookup(walk.links[i].name);
        if (!row)
        {
            continue;
        }
        walk.links[i].read        = true;
        walk.links[i].status      = row->status;
        walk.links[i].destination = row->destination;
        if (!walk.firstMove && field_hold::MoveSetting(row->status))
        {
            walk.firstMove = i;
        }
        if (field_hold::PathEnd(row->status))
        {
            continue;  // the member stands again: this branch of the path ends here
        }
        for (std::wstring& next : row->next)
        {
            const bool seen = next.empty() || std::any_of(walk.links.begin(), walk.links.end(),
                                                          [&next](const Link& link) { return link.name == next; });
            if (seen)
            {
                continue;
            }
            if (walk.links.size() >= maxLinks)
            {
                walk.truncated = true;
                break;
            }
            walk.links.push_back(Link{.name = std::move(next), .parent = i});
        }
    }
    return walk;
}

/// The links from the first to `at`, parent by parent, in walking order.
inline std::vector<std::size_t> Trail(const Walk& walk, std::size_t at)
{
    std::vector<std::size_t> trail;
    if (at >= walk.links.size())
    {
        return trail;
    }
    for (;;)
    {
        trail.push_back(at);
        if (at == 0)
        {
            break;
        }
        at = walk.links[at].parent;
    }
    std::reverse(trail.begin(), trail.end());
    return trail;
}

}  // namespace lss::players::state_path
