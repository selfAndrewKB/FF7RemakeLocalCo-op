#include "world/run_catch_up.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/game_vectors.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/field_root.h"
#include "players/lead_gate.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "world/actor_place.h"
#include "world/catch_up_move.h"
#include "world/field_targets.h"
#include "world/story_box_rules.h"
#include "world/story_boxes.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace lss::world::run_catch_up {

namespace {

using platform::GameVector;
namespace rules = story_box_rules;

constexpr float kBesideCm = catch_up_move::kFollowerCm;  // from player 1's character
constexpr float kPi       = 3.14159265358979f;

// Set at install, then only read.
bool           g_installed = false;

std::atomic<bool> g_on{true};  // the console's switch
// Game thread.
std::uint32_t g_moved = 0, g_canRun = 0, g_onePast = 0, g_noPoint = 0, g_busy = 0, g_inScope = 0, g_noShape = 0;
std::wstring  g_last  = L"none yet";

std::wstring Reading()
{
    return std::format(L"{}; brought back {}; left (its run passes through its box) {}, player 1 past the box too {}, no navmesh "
                       L"point {}, in a field action {}, inside a player's scope {}, no box shape {}; last: {}",
                       !g_installed ? L"not installed" : g_on.load() ? L"on" : L"off", g_moved, g_canRun, g_onePast, g_noPoint,
                       g_busy, g_inScope, g_noShape, g_last);
}

}  // namespace

bool Install(const platform::GameImage&) noexcept
{
    if (!catch_up_move::Installed() || !players::field_root::Installed())
    {
        platform::Log(L"run catch-up: an engine check failed; a companion taken past its own story box stays where it is "
                      L"when its run goes to its AI");
        return false;
    }
    g_installed = true;
    control::Register({.name  = L"run-catch-up",
                       .shape = control::Shape::OnOff,
                       .help  = L"a companion a further player took past its own story box is brought beside player 1's "
                                L"character when the level's run through that box goes to its AI (on at start); off: it runs "
                                L"from where it stands, as before",
                       .set   = [](const control::Value& value) {
                           g_on.store(std::get<bool>(value));
                           platform::Log(L"run catch-up: {}", g_on.load() ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_on.load()}; }});
    control::Register({.name  = L"world.run-catch-up",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) companions brought back for their run into their own story box, and why not",
                       .get   = [] {
                           try
                           {
                               return control::Value{Reading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
    platform::Log(L"run catch-up: a companion a further player took past its own story box is brought beside player 1's "
                  L"character when the level's run through it goes to its AI (run-catch-up on)");
    return true;
}

bool BeforeGuide(int seat, void* member) noexcept
try
{
    if (!g_installed || !g_on.load() || member == nullptr)
    {
        return false;
    }
    players::lead_gate::Mark mark;
    std::int64_t             box = 0;
    if (!players::lead_gate::OwnBoxRun(member, mark, box))
    {
        return false;  // a guide for another reason: as before
    }
    const std::wstring who = std::format(L"{}'s member", players::SeatName(seat));
    const std::wstring where = std::format(L"its run to {} through {}", players::lead_gate::PointName(mark.point),
                                           field_targets::NameOf(static_cast<std::uint64_t>(box)));
    rules::Box shape{};
    if (!story_boxes::BoxShape(static_cast<std::uint64_t>(box), shape))
    {
        ++g_noShape;
        platform::LogDebug(L"run catch-up: {}: {}: the box's shape is not read; not moved", who, where);
        return false;
    }
    float at[3] = {};
    if (!actor_place::ActorLocation(member, at))
    {
        return false;
    }
    if (rules::RunCrosses(shape, at, mark.at))
    {
        ++g_canRun;
        platform::LogDebug(L"run catch-up: {}: {} passes through the box from where it stands; not moved", who, where);
        return false;
    }
    using namespace players::scopes;
    if (OpenPlayer(Leader | Primary | Operator) != 0)
    {
        ++g_inScope;  // "the leader" would be a seat's character
        platform::LogDebug(L"run catch-up: {}: asked inside a player's scope; not moved", who);
        return false;
    }
    void* const one   = players::field_root::PartyLeader(facts::kLeaderCurrent);
    float       oneAt[3] = {};
    if (one == nullptr || one == member || !actor_place::ActorLocation(one, oneAt))
    {
        return false;
    }
    if (!rules::RunCrosses(shape, oneAt, mark.at))
    {
        ++g_onePast;
        g_last = std::format(L"{}: {}: player 1's character is past the box too; not moved", who, where);
        platform::Log(L"run catch-up: {}", g_last);
        return false;
    }
    if (catch_up_move::InFieldAction(member))
    {
        ++g_busy;
        platform::LogDebug(L"run catch-up: {}: in a field action; not moved", who);
        return false;
    }
    // Toward the run's point from player 1's character, then 35 degrees to either side, then behind it: the first navmesh
    // point the run passes the box from and no other player's companion stands on (two members may run at once: the Ch. 7
    // and Ch. 9 pairs).
    const GameVector from{oneAt[0], oneAt[1], oneAt[2]};
    const float      base = std::atan2(mark.at[1] - oneAt[1], mark.at[0] - oneAt[0]);
    if (!std::isfinite(base))
    {
        return false;
    }
    constexpr std::array<float, 4>          kTurns{0.0f, 35.0f, -35.0f, 180.0f};
    constexpr std::array<const wchar_t*, 4> kSides{L"toward the run's point", L"to one side of the run", L"to the other side",
                                                   L"behind it"};
    GameVector     point{};
    const wchar_t* side = nullptr;
    for (std::size_t i = 0; i < kTurns.size() && side == nullptr; ++i)
    {
        const float a = base + kTurns[i] * kPi / 180.0f;
        GameVector  candidate{};
        if (!catch_up_move::NavPoint(from, GameVector{std::cos(a), std::sin(a), 0.0f}, kBesideCm, candidate))
        {
            continue;
        }
        const float c[3] = {candidate.x, candidate.y, candidate.z};
        if (rules::RunCrosses(shape, c, mark.at) && !catch_up_move::SpotTaken(candidate, member))
        {
            point = candidate;
            side  = kSides[i];
        }
    }
    const float away = std::hypot(at[0] - oneAt[0], at[1] - oneAt[1]) / 100.0f;
    if (side == nullptr)
    {
        ++g_noPoint;
        g_last = std::format(L"{}: {}: {:.1f} m from player 1's character, no navmesh point beside it that the run passes the box "
                             L"from; not moved",
                             who, where, away);
        platform::Log(L"run catch-up: {}", g_last);
        return false;
    }
    const float location[3] = {point.x, point.y, point.z};
    if (!catch_up_move::Move(member, point, std::atan2(mark.at[1] - point.y, mark.at[0] - point.x) * 180.0f / kPi))
    {
        platform::LogWarning(L"run catch-up: the move of {} raised; it may stand where it was", who);
        return false;
    }
    const int waiting = story_boxes::AwaitRun(member, location, mark.at);
    ++g_moved;
    g_last = std::format(L"{}: {} could not pass through the box from {:.1f} m away from player 1's character: brought 1.5 m from player 1's "
                         L"character, {}; {} held-back story enter(s) wait for its own run",
                         who, where, away, side, waiting);
    platform::Log(L"run catch-up: {}", g_last);
    return true;
}
catch (...)
{
    platform::Log(L"run catch-up: an exception; not moved");
    return false;
}

}  // namespace lss::world::run_catch_up
