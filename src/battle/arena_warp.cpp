#include "battle/arena_warp.h"

#include "battle/arena_warp_rules.h"
#include "battle/member_state.h"
#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/action_key.h"
#include "players/actor_fields.h"
#include "players/field_root.h"
#include "players/game_state.h"
#include "players/party_member.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "world/catch_up_move.h"
#include "world/party_warp.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>

#include <Windows.h>

namespace lss::battle::arena_warp {

namespace {

using platform::Read;
using platform::ReadPointer;
using players::SeatName;
namespace game_state = players::game_state;

using WarpFn    = bool (*)(void* component, float distance);
using InsideFn  = bool (*)(void* battleSystem, void* character);
using FindRowFn = void* (*)(void* table, std::uint64_t name);

// Set at install, then only read.
std::uintptr_t                 g_dashKey = 0, g_distanceName = 0, g_gameParams = 0;
players::action_key::Getter    g_pressed = nullptr;
WarpFn                         g_warp    = nullptr;
InsideFn                       g_inside  = nullptr;

// Game thread. Index = seat ([0] unused).
struct Seat
{
    arena_warp_rules::DoubleClick click;          // the Dash presses in a battle (battle/arena_warp_rules.h)
    bool                          asked = false;  // a double click waits for the next engine tick
};
std::array<Seat, players::scopes::kMaxPlayers> g_seats;
std::uint32_t g_presses = 0, g_fieldPresses = 0, g_warps = 0, g_fromInside = 0, g_refused = 0, g_noPoint = 0;
std::uint64_t g_reads = 0, g_readsFailed = 0;  // the Dash reads made, and those the game's getter could not answer

using players::field_root::BattleSystem;
using players::field_root::InBattle;

/// The game's warp distance: its GameParam row, looked up as its leader switch does, else its default.
float Distance() noexcept
{
    std::uint64_t name = 0;
    if (!Read(reinterpret_cast<const void*>(g_distanceName), name) || name == 0)
    {
        return facts::kArenaWarpDefaultCm;  // the game names the row at its first leader switch's warp
    }
    for (int i = 0; i < 3; ++i)
    {
        void* table = ReadPointer(reinterpret_cast<const void*>(g_gameParams + 8 * i), 0);
        void* vtable = table != nullptr ? ReadPointer(table, 0) : nullptr;
        auto  find   = vtable != nullptr ? reinterpret_cast<FindRowFn>(ReadPointer(vtable, facts::kTableFindRowSlot)) : nullptr;
        void* row    = nullptr;
        float value  = 0.0f;
        // A row found is the game's value; a zero or negative one (a row that turns the warp off) is not taken.
        if (find != nullptr && platform::CallGuarded(find, row, table, name) && row != nullptr &&
            Read(static_cast<const std::uint8_t*>(row) + facts::kGameParamFloat, value) && value > 0.0f)
        {
            return value;
        }
    }
    return facts::kArenaWarpDefaultCm;
}

/// Why the seat's member may not be brought in now (null: it may).
const wchar_t* Refusal(void* battleSystem, void* member) noexcept
{
    if (game_state::Current().paused || game_state::MenuOwnsInput())
    {
        return L"the game is paused or a menu is up";
    }
    if (players::party_member::PushButtonLocked())
    {
        return L"a push-button event (a switch, a lever, a terminal) holds the party";
    }
    std::uint8_t  noRecovery = 1;
    std::uint32_t ready = 0;
    if (!Read(static_cast<const std::uint8_t*>(battleSystem) + facts::kBattleNoRecovery, noRecovery) || noRecovery != 0 ||
        !Read(static_cast<const std::uint8_t*>(battleSystem) + facts::kBattleRecoveryReady, ready) || ready == 0)
    {
        return L"this battle brings no member back";
    }
    const auto state = member_state::ActionState(member);
    if (!state)
    {
        return L"its battle state is unread";
    }
    if (*state == facts::kBattleCompStateKnockedOut)
    {
        return L"it is down (the game brings it in when revived)";
    }
    if (member_state::InLockedAction(member))
    {
        return L"it is in the middle of an action";
    }
    if (world::catch_up_move::FieldActionUnderway(member))
    {
        return L"it is in a field action (a ladder, a crawl)";
    }
    return nullptr;
}

void Run(int seat) noexcept
{
    void* const battleSystem = BattleSystem();
    void* const member       = players::scopes::Member(seat);
    if (!InBattle() || battleSystem == nullptr)
    {
        return;  // the battle ended in between
    }
    if (member == nullptr || !players::weak_object::Live(member))
    {
        return;
    }
    if (const wchar_t* why = Refusal(battleSystem, member))
    {
        ++g_refused;  // the player's own ask: answered at Info
        platform::Log(L"arena warp: {} clicked the left stick twice; its character is not brought into the fight: {} "
                      L"(field lock {:#x})",
                      SeatName(seat), why, players::party_member::ControlLockMask());
        return;
    }
    // The area test is not a gate: a character can stand inside the fight's area and still be cut off from it (a train
    // car's far side). It is read for the log only.
    bool       inside     = false;
    const bool insideRead = g_inside != nullptr && platform::CallGuarded(g_inside, inside, battleSystem, member);
    const float distance = Distance();
    bool        moved    = false;
    void* const component = players::actor_fields::BattleComponent(member);
    if (component == nullptr || !platform::CallGuarded(g_warp, moved, component, distance))
    {
        platform::LogWarning(L"arena warp: the game's warp raised for {}'s character; it may stand where it was", SeatName(seat));
        return;
    }
    if (!moved)
    {
        ++g_noPoint;
        platform::Log(L"arena warp: {}'s character could not be brought into the fight (the game's warp moved nothing: no "
                      L"navmesh point {:.1f} m in front of player 1's character, or it is the party leader)",
                      SeatName(seat), distance / 100.0f);
        return;
    }
    ++g_warps;
    if (insideRead && inside)
    {
        ++g_fromInside;
    }
    platform::Log(L"arena warp: {}'s character is brought in front of player 1's character ({:.1f} m; the game's area test: {}; "
                  L"field lock {:#x})",
                  SeatName(seat), distance / 100.0f, !insideRead ? L"unread" : inside ? L"inside" : L"outside",
                  players::party_member::ControlLockMask());
}

std::wstring Reading()
{
    return std::format(L"{}; Dash presses seen in a battle {}, brought in {} (from inside the fight's area {}), refused {}, nothing moved {}; "
                       L"Dash presses seen in the field {} (their double clicks: world.party-warp); the further players' Dash "
                       L"read {} times, unanswered {}",
                       g_warp != nullptr ? L"on" : L"off", g_presses, g_warps, g_fromInside, g_refused, g_noPoint,
                       g_fieldPresses, g_reads, g_readsFailed);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto dash     = image.Rebase(facts::kDashKey, 24);
    const auto pressed  = platform::FindFunction(image, facts::kActionPressedByKey);
    const auto warp     = platform::FindFunction(image, facts::kArenaWarp);
    const auto inside   = platform::FindFunction(image, facts::kInsideBattleArea);
    const auto name     = image.Rebase(facts::kNameWarpDistanceCache, sizeof(std::uint64_t));
    const auto params   = image.Rebase(facts::kTableGameParam, 3 * sizeof(void*));
    if (!dash || !pressed || !warp || !players::field_root::Installed() || !name || !params)
    {
        platform::Log(L"arena warp: an engine check failed; a double click of the left stick does not bring a character into "
                      L"the fight");
        return false;
    }
    g_dashKey      = *dash;
    g_pressed      = reinterpret_cast<players::action_key::Getter>(*pressed);
    g_inside       = inside ? reinterpret_cast<InsideFn>(*inside) : nullptr;  // the log's only; optional
    g_distanceName = *name;
    g_gameParams   = *params;
    g_warp         = reinterpret_cast<WarpFn>(*warp);  // last: Tick and ReadPress test it
    control::Register({.name  = L"arena-warp",
                       .shape = control::Shape::Text,
                       .help  = L"further players' characters brought into the fight on a double click of the left stick: "
                                L"Dash presses seen in a battle (one per click), brought in (from inside the fight's area, by the game's test), refused (paused "
                                L"or a menu up, a scripted event, a battle with no recovery, down, mid-action, a field "
                                L"action), nothing moved (no navmesh point, or the party leader); and the Dash presses seen "
                                L"in the field, where a double click brings the character back to player 1's",
                       .get   = [] { return control::Value{Reading()}; }});
    platform::Log(L"arena warp: installed; player 2 or 3 double-clicking the left stick in a battle, inside the fight's area or not, "
                  L"is brought in front of player 1's character");
    return true;
}

void ReadPress(int seat) noexcept
{
    if (g_warp == nullptr || !players::SeatBuilt(seat))
    {
        return;
    }
    ++g_reads;
    bool pressed = false;
    if (!players::action_key::Test(g_pressed, g_dashKey, pressed))
    {
        ++g_readsFailed;
        return;
    }
    if (!pressed)
    {
        return;
    }
    ++(InBattle() ? g_presses : g_fieldPresses);  // one click adds one: a press seen once per click
    Seat& s = g_seats[seat];
    if (s.click.Press(GetTickCount64()))
    {
        s.asked = true;
    }
}

void Tick() noexcept
{
    if (g_warp == nullptr)
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Seat& s = g_seats[seat];
        if (!s.asked)
        {
            continue;
        }
        s.asked = false;
        if (players::scopes::OpenPlayer(players::scopes::All) != 0)
        {
            continue;  // never expected at the engine tick; the warp would read a seat's character as the leader
        }
        if (InBattle())
        {
            Run(seat);
        }
        else
        {
            world::party_warp::Bring(seat);  // the field's answer to the same ask
        }
    }
}

void Forget(int seat) noexcept
{
    if (players::SeatBuilt(seat))
    {
        g_seats[seat] = {};
    }
}

}  // namespace lss::battle::arena_warp
