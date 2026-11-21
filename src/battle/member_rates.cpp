#include "battle/member_rates.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/seat_camera.h"
#include "players/scopes.h"
#include "players/seats.h"

#include <array>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::battle::member_rates {

namespace {

using platform::Read;

using PassiveAtbFn = void (*)(void* component, float deltaTime);
using UcpcFlagFn   = char (*)(void* component, std::int64_t a2, std::int64_t a3);
using StaggerFn    = char (*)(float* burst, void* attacker, void* hit);
using SteerFn      = char (*)(void* component, float deltaTime);

std::array<platform::VTableEntry, facts::kBattleCompVTables.size()> g_passiveSlots;
std::array<platform::VTableEntry, facts::kBattleCompVTables.size()> g_flagSlots;
platform::CallRedirect                                             g_staggerCall;
platform::CallRedirect                                             g_steerCall;
PassiveAtbFn                                                       g_passive = nullptr;
UcpcFlagFn                                                         g_flag    = nullptr;

// Game thread.
std::array<void*, players::scopes::kMaxPlayers> g_components{};  // each seat's member's battle component this tick; [0] unused
bool   g_enabled   = true;
constexpr int kLevers = 4;  // the passive ATB tick, the ability flag, the stagger hit, the turn during a roll
std::uint32_t g_offThread[kLevers]{};
bool          g_offThreadLogged[kLevers]{};
bool          g_appliedLogged[kLevers]{};
std::array<bool, players::scopes::kMaxPlayers> g_turnOpen{};  // per seat: its member's turn window was open last call

/// The seat whose member's battle component this is (0 none).
int SeatOfComponent(const void* component) noexcept
{
    for (int seat = 1; component != nullptr && seat <= players::kSeatsBuilt; ++seat)
    {
        if (g_components[static_cast<std::size_t>(seat)] == component)
        {
            return seat;
        }
    }
    return 0;
}

/// The seat whose member this character is (0 none).
int SeatOfMember(const void* character) noexcept
{
    for (int seat = 1; character != nullptr && seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::scopes::Member(seat) == character)
        {
            return seat;
        }
    }
    return 0;
}

/// Whether this call is a seat's member's and may run in that seat's leader scope (the game thread only).
bool Scoped(int seat, int lever, const wchar_t* what) noexcept
{
    if (seat == 0 || !g_enabled)
    {
        return false;
    }
    if (!platform::OnGameThread())
    {
        ++g_offThread[lever];
        if (!g_offThreadLogged[lever])
        {
            g_offThreadLogged[lever] = true;
            platform::LogWarning(L"member rates: {} for {}'s member ran off the game thread; the AI-ally rate stays there", what,
                                 players::SeatName(seat));
        }
        return false;
    }
    if (!g_appliedLogged[lever])
    {
        g_appliedLogged[lever] = true;
        platform::LogDebug(L"member rates: {} runs for {}'s member as the player's", what, players::SeatName(seat));
    }
    return true;
}

void PassiveAtb(void* component, float deltaTime)
{
    if (const int seat = SeatOfComponent(component); Scoped(seat, 0, L"the passive ATB tick"))
    {
        players::scopes::Scope leader(seat, players::scopes::Leader);
        g_passive(component, deltaTime);
        return;
    }
    g_passive(component, deltaTime);
}

char UcpcFlag(void* component, std::int64_t a2, std::int64_t a3)
{
    if (const int seat = SeatOfComponent(component); Scoped(seat, 1, L"the ability flag"))
    {
        players::scopes::Scope leader(seat, players::scopes::Leader);
        return g_flag(component, a2, a3);
    }
    return g_flag(component, a2, a3);
}

char Stagger(float* burst, void* attacker, void* hit)
{
    const auto original = reinterpret_cast<StaggerFn>(g_staggerCall.Original());
    if (const int seat = SeatOfMember(attacker); Scoped(seat, 2, L"the stagger hit"))
    {
        players::scopes::Scope leader(seat, players::scopes::Leader);
        return original(burst, attacker, hit);
    }
    return original(burst, attacker, hit);
}

/// The turn during an action: a seat's member turns by that player's stick in that player's camera; the window's
/// opening is logged once per window (an event: Debug).
char Steer(void* component, float deltaTime)
{
    const auto original = reinterpret_cast<SteerFn>(g_steerCall.Original());
    const int  seat     = SeatOfComponent(component);
    if (seat == 0)
    {
        return original(component, deltaTime);
    }
    float              rate = 0.0f;
    const std::uint8_t* c   = static_cast<const std::uint8_t*>(component);
    const bool          open = Read(c + facts::kBattleCompTurnRate, rate) && rate != 0.0f;
    if (!open)
    {
        if (platform::OnGameThread())
        {
            g_turnOpen[static_cast<std::size_t>(seat)] = false;
        }
        return original(component, deltaTime);  // closed: the game's function returns at its rate test
    }
    if (!Scoped(seat, 3, L"the turn during a roll"))
    {
        return original(component, deltaTime);  // off the game thread (counted) or off: the game's own reads
    }
    const players::scopes::Scope primary(seat, players::scopes::Primary);
    if (!g_turnOpen[static_cast<std::size_t>(seat)])
    {
        std::uint8_t state = 0, force = 0;
        Read(c + facts::kBattleCompActionState, state);
        Read(c + facts::kBattleCompTurnForce, force);
        platform::LogDebug(L"member rates: {}'s member may turn by {} (rate {:.2f}, force {}, action state {}{})", players::SeatName(seat),
                           primary.Held() ? L"that player's stick and camera" : L"player 1's stick and camera (ITS CAMERA BASIS IS NOT BOUND)",
                           rate, force, state, state == facts::kBattleCompStateDodge ? L": a roll" : L"");
    }
    g_turnOpen[static_cast<std::size_t>(seat)] = true;
    if (!primary.Held())
    {
        return original(component, deltaTime);  // never the seat's stick against player 1's camera
    }
    const players::seat_camera::StickScope stick(static_cast<std::uint32_t>(seat));
    return original(component, deltaTime);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto passive = platform::FindFunction(image, facts::kBattleCompTickPassiveAtb);
    const auto flag    = platform::FindFunction(image, facts::kBattleCompSetUcpcFlag);
    const auto stagger = platform::FindFunction(image, facts::kBattleStaggerRewards);
    const auto site    = image.Rebase(facts::kBattleStaggerRewardsCall, 5);
    const auto steer   = platform::FindFunction(image, facts::kBattleCompSteerToStick);
    const auto steerAt = image.Rebase(facts::kBattleCompSteerToStickCall, 5);
    if (!passive || !flag || !stagger || !site)
    {
        platform::Log(L"member rates: an engine check failed; the further players' members keep the AI-ally rates");
        return false;
    }
    g_passive = reinterpret_cast<PassiveAtbFn>(*passive);
    g_flag    = reinterpret_cast<UcpcFlagFn>(*flag);
    // The turn during a roll stands alone: if its check fails the rates still go in, and the rolls keep player 1's stick.
    if (!steer || !steerAt || !g_steerCall.Install(*steerAt, *steer, reinterpret_cast<std::uintptr_t>(&Steer)))
    {
        platform::Log(L"member rates: the turn during a roll is not what the research read; the further players' rolls keep turning by player 1's stick");
    }
    bool installed = g_staggerCall.Install(*site, *stagger, reinterpret_cast<std::uintptr_t>(&Stagger));
    for (std::size_t i = 0; installed && i < facts::kBattleCompVTables.size(); ++i)
    {
        const std::uintptr_t vtable = facts::kBattleCompVTables[i];
        // One component class replaces the passive ATB tick with its own (no ATB): its slot is left alone.
        const auto held = image.ReadPointer(vtable + facts::kBattleCompPassiveAtbSlot * 8);
        if (held && image.Rebase(facts::kBattleCompTickPassiveAtb.address) == *held)
        {
            const platform::SlotFact passiveSlot{L"BattlePlayerComp::TickPassiveATB (slot 99)", vtable + facts::kBattleCompPassiveAtbSlot * 8,
                                                 facts::kBattleCompTickPassiveAtb};
            installed = platform::ReplaceSlot(image, passiveSlot, reinterpret_cast<std::uintptr_t>(&PassiveAtb), g_passiveSlots[i]);
        }
        const platform::SlotFact flagSlot{L"BattlePlayerComp::SetUCPCAbilityFlag (slot 233)", vtable + facts::kBattleCompUcpcFlagSlot * 8,
                                          facts::kBattleCompSetUcpcFlag};
        installed = installed && platform::ReplaceSlot(image, flagSlot, reinterpret_cast<std::uintptr_t>(&UcpcFlag), g_flagSlots[i]);
    }
    if (!installed)
    {
        Remove();
        platform::Log(L"member rates: a slot or call could not be replaced; the further players' members keep the AI-ally rates");
        return false;
    }
    control::Register({.name  = L"member-rates",
                       .shape = control::Shape::OnOff,
                       .help  = L"each further player's member fills ATB, flags abilities and staggers at the player's rates, and turns by "
                                L"that player's stick during a roll (battle/member_rates.h; on)",
                       .set   = [](const control::Value& value) {
                           g_enabled = std::get<bool>(value);
                           platform::Log(L"member rates {}", g_enabled ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    platform::Log(L"member rates: installed; each further player's member's ATB, ability flag and staggers run as the player's{}",
                  g_steerCall.IsInstalled() ? L", its turn during a roll by the player's own stick and camera" : L"");
    return true;
}

void Remove() noexcept
{
    for (auto& slot : g_flagSlots)
    {
        slot.Restore();
    }
    for (auto& slot : g_passiveSlots)
    {
        slot.Restore();
    }
    g_staggerCall.Restore();
    g_steerCall.Restore();
}

void Tick() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        void* member    = players::scopes::Member(seat);
        void* component = nullptr;
        g_components[static_cast<std::size_t>(seat)] =
            member != nullptr && Read(static_cast<const std::uint8_t*>(member) + facts::kCharacterBattleComponent, component)
                ? component
                : nullptr;
    }
}

}  // namespace lss::battle::member_rates
