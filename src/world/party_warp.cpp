#include "world/party_warp.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/game_vectors.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/field_root.h"
#include "players/game_state.h"
#include "players/party_member.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "world/actor_place.h"
#include "world/catch_up_move.h"
#include "world/party_warp_rules.h"

#include <cstdint>
#include <format>
#include <string>

namespace lss::world::party_warp {

namespace {

using platform::GameVector;
using players::SeatName;
namespace rules = party_warp_rules;

// Game thread.
bool          g_installed = false;
std::uint32_t g_asks = 0, g_moved = 0, g_refused = 0, g_noPoint = 0, g_raised = 0;
std::wstring  g_last = L"none yet";

std::wstring Reading()
{
    return std::format(L"{}; asks {}, brought back {}, refused {}, no navmesh point {}, the move raised {}; last: {}",
                       g_installed ? L"on" : L"not installed", g_asks, g_moved, g_refused, g_noPoint, g_raised, g_last);
}

/// The answer to one ask, for the log and the reading.
void Answer(std::wstring text)
{
    g_last = std::move(text);
    platform::Log(L"party warp: {}", g_last);
}

}  // namespace

bool Install(const platform::GameImage&) noexcept
{
    if (!catch_up_move::Installed() || !players::field_root::Installed())
    {
        platform::Log(L"party warp: an engine check failed; a further player's double click of the left stick brings no "
                      L"character back to player 1's in the field");
        return false;
    }
    g_installed = true;
    control::Register({.name  = L"party-warp",
                       .shape = control::Shape::Integer,
                       .min   = 2,
                       .max   = players::kSeatsBuilt + 1,
                       .help  = L"brings that player's character back beside player 1's in the field, as their double click of "
                                L"the left stick does (2 or 3)",
                       .set   = [](const control::Value& value) {
                           const auto player = std::get<std::int64_t>(value);
                           if (!Bring(static_cast<int>(player) - 1))
                           {
                               return control::Outcome{.ok = false, .detail = L"not a further player's seat"};
                           }
                           return control::Outcome{.detail = g_last};
                       }});
    control::Register({.name  = L"world.party-warp",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) further players' characters brought back beside player 1's in the field: asks, "
                                L"brought back, refused (paused, a scene or event, a scripted hold, a field action, already beside), no navmesh "
                                L"point, the move raised, and the last answer",
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
    platform::Log(L"party warp: installed; a further player who double-clicks the left stick in the field is brought back "
                  L"beside player 1's character");
    return true;
}

bool Bring(int seat) noexcept
try
{
    if (!g_installed || !players::SeatBuilt(seat))
    {
        return false;
    }
    ++g_asks;
    using namespace players::scopes;
    void* const member = Member(seat);
    if (member == nullptr || !players::weak_object::Live(member))
    {
        ++g_refused;
        Answer(std::format(L"{} asked to be brought back; it plays no character", SeatName(seat)));
        return true;
    }
    if (OpenPlayer(Leader | Primary | Operator) != 0)
    {
        ++g_refused;  // never expected at the engine tick: "the leader" would be a seat's character
        Answer(std::format(L"{} asked to be brought back inside a player's scope; not moved", SeatName(seat)));
        return true;
    }
    // The gates that need neither character first, so a pause or a scene is named as such whatever else reads.
    rules::Facts seen{
        .paused           = players::game_state::Current().paused || players::game_state::MenuOwnsInput(),
        .fieldLock        = players::field_root::ControlLockMask(),
        .leaderWithPlayer = players::party_member::LeaderWithPlayer(),
    };
    if (const wchar_t* why = rules::Refusal(seen))
    {
        ++g_refused;
        Answer(std::format(L"{} asked to be brought back; not moved: {} (field lock {:#x})", SeatName(seat), why, seen.fieldLock));
        return true;
    }
    void* const one = players::field_root::PartyLeader(facts::kLeaderCurrent);
    GameVector  at{}, oneAt{};
    float       yaw = 0.0f;
    if (one == nullptr || one == member || !actor_place::ActorLocation(member, at) || !actor_place::ActorLocation(one, oneAt) ||
        !actor_place::ActorYaw(one, yaw))
    {
        ++g_refused;
        Answer(std::format(L"{} asked to be brought back; player 1's character or its own could not be read", SeatName(seat)));
        return true;
    }
    const std::uint8_t mode = players::party_member::ActorMode(member);
    seen.memberWithPlayer   = !players::party_member::HandedToAi(member);
    seen.memberInField      = mode == facts::kActorModeField || mode == facts::kActorModeCaution;
    seen.memberBusy         = catch_up_move::FieldActionUnderway(member);
    seen.leaderBusy         = catch_up_move::FieldActionUnderway(one);
    seen.beside             = rules::Beside(actor_place::FlatDistance(at, oneAt), at.z - oneAt.z);
    const float away        = actor_place::FlatDistance(at, oneAt) / 100.0f;
    if (const wchar_t* why = rules::Refusal(seen))
    {
        ++g_refused;
        Answer(std::format(L"{} asked to be brought back from {:.1f} m away; not moved: {} (its mode {})", SeatName(seat), away, why,
                           mode));
        return true;
    }
    const auto  tries = rules::Spots(yaw);
    GameVector  point{};
    std::size_t used = tries.size();
    for (std::size_t i = 0; i < tries.size() && used == tries.size(); ++i)
    {
        GameVector candidate{};
        if (catch_up_move::NavPoint(oneAt, tries[i], catch_up_move::kFollowerCm, candidate) &&
            !catch_up_move::SpotTaken(candidate, member))
        {
            point = candidate;
            used  = i;
        }
    }
    if (used == tries.size())
    {
        ++g_noPoint;
        Answer(std::format(L"{} asked to be brought back from {:.1f} m away; no free navmesh point beside player 1's character: "
                           L"not moved",
                           SeatName(seat), away));
        return true;
    }
    if (!catch_up_move::Move(member, point, yaw))
    {
        ++g_raised;
        g_last = std::format(L"the move of {}'s character raised; it may stand where it was", SeatName(seat));
        platform::LogWarning(L"party warp: {}", g_last);
        return true;
    }
    ++g_moved;
    Answer(std::format(L"{}'s character was {:.1f} m from player 1's and is brought {:.1f} m from it, {}", SeatName(seat), away,
                       catch_up_move::kFollowerCm / 100.0f, rules::SpotName(used)));
    return true;
}
catch (...)
{
    ++g_raised;
    platform::LogWarning(L"party warp: an exception; not moved");
    try
    {
        g_last = L"an exception; not moved";
    }
    catch (...)
    {
    }
    return true;
}

}  // namespace lss::world::party_warp
