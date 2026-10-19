#include "battle/member_rates.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/scopes.h"

#include <array>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::battle::member_rates {

namespace {

using PassiveAtbFn = void (*)(void* component, float deltaTime);
using UcpcFlagFn   = char (*)(void* component, std::int64_t a2, std::int64_t a3);
using StaggerFn    = char (*)(float* burst, void* attacker, void* hit);

constexpr int kPlayer = 1;

std::array<platform::VTableEntry, facts::kBattleCompVTables.size()> g_passiveSlots;
std::array<platform::VTableEntry, facts::kBattleCompVTables.size()> g_flagSlots;
platform::CallRedirect                                             g_staggerCall;
PassiveAtbFn                                                       g_passive = nullptr;
UcpcFlagFn                                                         g_flag    = nullptr;

// Game thread.
DWORD  g_thread    = 0;
void*  g_component = nullptr;  // player 2's member's battle component this tick
bool   g_enabled   = true;
std::uint32_t g_offThread[3]{};
bool          g_offThreadLogged[3]{};
bool          g_appliedLogged[3]{};

template <class T>
bool Read(const void* address, T& out) noexcept
{
    return address != nullptr && platform::CopyGuarded(&out, address, sizeof(T));
}

/// Whether this call is player 2's member's and may run in its leader scope (the game thread only).
bool Scoped(bool ours, int lever, const wchar_t* what) noexcept
{
    if (!ours || !g_enabled)
    {
        return false;
    }
    if (GetCurrentThreadId() != g_thread)
    {
        ++g_offThread[lever];
        if (!g_offThreadLogged[lever])
        {
            g_offThreadLogged[lever] = true;
            platform::LogWarning(L"member rates: {} for player 2's member ran off the game thread; the AI-ally rate stays there", what);
        }
        return false;
    }
    if (!g_appliedLogged[lever])
    {
        g_appliedLogged[lever] = true;
        platform::LogDebug(L"member rates: {} runs for player 2's member as the player's", what);
    }
    return true;
}

void PassiveAtb(void* component, float deltaTime)
{
    if (Scoped(component == g_component, 0, L"the passive ATB tick"))
    {
        players::scopes::Scope leader(kPlayer, players::scopes::Leader);
        g_passive(component, deltaTime);
        return;
    }
    g_passive(component, deltaTime);
}

char UcpcFlag(void* component, std::int64_t a2, std::int64_t a3)
{
    if (Scoped(component == g_component, 1, L"the ability flag"))
    {
        players::scopes::Scope leader(kPlayer, players::scopes::Leader);
        return g_flag(component, a2, a3);
    }
    return g_flag(component, a2, a3);
}

char Stagger(float* burst, void* attacker, void* hit)
{
    const auto original = reinterpret_cast<StaggerFn>(g_staggerCall.Original());
    if (Scoped(attacker != nullptr && attacker == players::scopes::Member(kPlayer), 2, L"the stagger hit"))
    {
        players::scopes::Scope leader(kPlayer, players::scopes::Leader);
        return original(burst, attacker, hit);
    }
    return original(burst, attacker, hit);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto passive = platform::FindFunction(image, facts::kBattleCompTickPassiveAtb);
    const auto flag    = platform::FindFunction(image, facts::kBattleCompSetUcpcFlag);
    const auto stagger = platform::FindFunction(image, facts::kBattleStaggerRewards);
    const auto site    = image.Rebase(facts::kBattleStaggerRewardsCall, 5);
    if (!passive || !flag || !stagger || !site)
    {
        platform::Log(L"member rates: an engine check failed; player 2's member keeps the AI-ally rates");
        return false;
    }
    g_passive = reinterpret_cast<PassiveAtbFn>(*passive);
    g_flag    = reinterpret_cast<UcpcFlagFn>(*flag);
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
        platform::Log(L"member rates: a slot or call could not be replaced; player 2's member keeps the AI-ally rates");
        return false;
    }
    control::Register({.name  = L"rates-two",
                       .shape = control::Shape::OnOff,
                       .help  = L"player 2's member fills ATB, flags abilities and staggers at the player's rates (battle/member_rates.h; on)",
                       .set   = [](const control::Value& value) {
                           g_enabled = std::get<bool>(value);
                           platform::Log(L"member rates {}", g_enabled ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    platform::Log(L"member rates: installed; player 2's member's ATB, ability flag and staggers run as the player's");
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
}

void Tick() noexcept
{
    g_thread      = GetCurrentThreadId();
    void* member  = players::scopes::Member(kPlayer);
    void* component = nullptr;
    g_component = member != nullptr && Read(static_cast<const std::uint8_t*>(member) + facts::kCharacterBattleComponent, component)
                      ? component
                      : nullptr;
}

}  // namespace lss::battle::member_rates
