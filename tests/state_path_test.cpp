// Host-side tests for players/state_path.h: a member's chain of level states, walked as the lead gate walks it.
// The chains are the ones the game printed live on 2026-10-27 (Aerith's guide home in Ch. 8, Barret's walk-ups to
// the terminal in Ch. 7), with the destinations made up.

#include "players/state_path.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

namespace sp = lss::players::state_path;

using Rows = std::map<std::wstring, sp::Row>;

sp::Walk WalkIn(const Rows& rows, const std::wstring& first, std::size_t maxLinks = 16)
{
    return sp::WalkFrom(first, [&rows](const std::wstring& name) -> std::optional<sp::Row> {
        const auto it = rows.find(name);
        return it == rows.end() ? std::nullopt : std::optional<sp::Row>(it->second);
    }, maxLinks);
}

void AerithsGuideHome()
{
    // Her first run's arrival leads to a stay (she waits for the player), then the next run: a further move ahead,
    // found at the second link. The walk goes on to the path's end and keeps every run's destination.
    const Rows rows = {
        {L"Lead_00", {L"Stay", {L"Lead_01"}, 0}},
        {L"Lead_01", {L"MoveToTarget", {L"Town5"}, 101}},
        {L"Town5", {L"", {L"PartyOut"}, 0}},
        {L"PartyOut", {L"Stay", {L"Wait"}, 0}},
        {L"Wait", {L"Animation", {L"Lead"}, 0}},
        {L"Lead", {L"MoveToTarget", {L"PV1400_Start"}, 102}},
        {L"PV1400_Start", {L"MoveToTarget", {L"PV1400"}, 103}},
        {L"PV1400", {L"Stay", {}, 0}},
    };
    const sp::Walk walk = WalkIn(rows, L"Lead_00");
    EXPECT(walk.firstMove.has_value() && *walk.firstMove == 1);
    EXPECT(walk.links.size() == 8);
    EXPECT(!walk.truncated);
    EXPECT(walk.links[1].destination == 101);
    EXPECT(walk.links[7].name == L"PV1400" && walk.links[7].read);
    EXPECT((sp::Trail(walk, 1) == std::vector<std::size_t>{0, 1}));
    EXPECT((sp::Trail(walk, 6) == std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 6}));
    // The nameless state (no Status) is read, and the walk passes through it.
    EXPECT(walk.links[2].read && walk.links[2].status.empty());
}

void BarretsWalkUps()
{
    // Ch. 7: a run to the terminal, a stay while he searches, a run to near it, a stay, a nameless wait, a run.
    const Rows rows = {
        {L"SearchTerminal", {L"Stay", {L"MoveToNearTerminal"}, 0}},
        {L"MoveToNearTerminal", {L"MoveToTarget", {L"WaitNearTerminal"}, 201}},
        {L"WaitNearTerminal", {L"Stay", {L"AfterBusshi"}, 0}},
        {L"AfterBusshi", {L"", {L"MovePre0300"}, 0}},
        {L"MovePre0300", {L"MoveToTarget", {}, 202}},
    };
    const sp::Walk first = WalkIn(rows, L"SearchTerminal");
    EXPECT(first.firstMove.has_value() && *first.firstMove == 1);
    EXPECT(first.links.size() == 5);
    const sp::Walk second = WalkIn(rows, L"WaitNearTerminal");
    EXPECT(second.firstMove.has_value() && *second.firstMove == 2);
    EXPECT(second.links[2].destination == 202);
}

void AStandEndsThePath()
{
    // One move ending in a stand: nothing further is the member's path. A move after a Stable is a later script's.
    const Rows rows = {
        {L"Stand", {L"Stable", {L"LaterRun"}, 0}},
        {L"Pause", {L"Stay", {L"Stand"}, 0}},
        {L"LaterRun", {L"MoveToTarget", {}, 301}},
    };
    const sp::Walk stand = WalkIn(rows, L"Stand");
    EXPECT(!stand.firstMove.has_value());
    EXPECT(stand.links.size() == 1);
    const sp::Walk pause = WalkIn(rows, L"Pause");
    EXPECT(!pause.firstMove.has_value());
    EXPECT(pause.links.size() == 2);
    // A Stable compared without case, as the game does.
    const Rows lower = {{L"Stand", {L"stable", {L"LaterRun"}, 0}}, {L"LaterRun", {L"Lead", {}, 0}}};
    EXPECT(!WalkIn(lower, L"Stand").firstMove.has_value());
}

void TheFirstStateMayBeAMove()
{
    // The arrival leads straight to another run: found at the first link (its own trail).
    const Rows rows = {{L"Run2", {L"Lead", {}, 401}}};
    const sp::Walk walk = WalkIn(rows, L"Run2");
    EXPECT(walk.firstMove.has_value() && *walk.firstMove == 0);
    EXPECT((sp::Trail(walk, 0) == std::vector<std::size_t>{0}));
}

void ALoopEnds()
{
    const Rows rows = {
        {L"A", {L"Stay", {L"B"}, 0}},
        {L"B", {L"Stay", {L"A", L"B", L""}, 0}},
    };
    const sp::Walk walk = WalkIn(rows, L"A");
    EXPECT(walk.links.size() == 2);
    EXPECT(!walk.truncated);
    EXPECT(!walk.firstMove.has_value());
}

void TheLimitHolds()
{
    // A fan-out wider than the limit: the walk stops adding and says so; a move beyond it is not seen.
    Rows rows;
    sp::Row hub{L"Stay", {}, 0};
    for (int i = 0; i < 20; ++i)
    {
        hub.next.push_back(L"Leaf" + std::to_wstring(i));
        rows[L"Leaf" + std::to_wstring(i)] = {L"Stay", {}, 0};
    }
    rows[L"Leaf19"] = {L"MoveToTarget", {}, 501};
    rows[L"Hub"]    = hub;
    const sp::Walk walk = WalkIn(rows, L"Hub", 16);
    EXPECT(walk.links.size() == 16);
    EXPECT(walk.truncated);
    EXPECT(!walk.firstMove.has_value());
}

void UnknownStates()
{
    // An unknown first state: one unread link, no move. An empty name: nothing walked.
    const Rows rows;
    const sp::Walk unknown = WalkIn(rows, L"Nowhere");
    EXPECT(unknown.links.size() == 1 && !unknown.links[0].read);
    EXPECT(!unknown.firstMove.has_value());
    EXPECT(WalkIn(rows, L"").links.empty());
    EXPECT(WalkIn(rows, L"Nowhere", 0).links.empty());
    EXPECT(sp::Trail(unknown, 5).empty());
}

void TheFirstMoveIsBreadthFirst()
{
    // Two branches: the nearer move is the one named, whatever the trigger order.
    const Rows rows = {
        {L"Fork", {L"Stay", {L"Far1", L"Near"}, 0}},
        {L"Far1", {L"Stay", {L"Far2"}, 0}},
        {L"Far2", {L"Lead", {}, 601}},
        {L"Near", {L"RouteMove", {}, 602}},
    };
    const sp::Walk walk = WalkIn(rows, L"Fork");
    EXPECT(walk.firstMove.has_value() && walk.links[*walk.firstMove].name == L"Near");
    EXPECT(walk.links.size() == 4);  // the far branch is still walked to its end
}

}  // namespace

int main()
{
    AerithsGuideHome();
    BarretsWalkUps();
    AStandEndsThePath();
    TheFirstStateMayBeAMove();
    ALoopEnds();
    TheLimitHolds();
    UnknownStates();
    TheFirstMoveIsBreadthFirst();
    if (g_failures != 0)
    {
        std::printf("state_path: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("state_path: all passed\n");
    return 0;
}
