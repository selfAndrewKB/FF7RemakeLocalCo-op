#include "battle/targeting_two.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/byte_scope.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/pad_buttons.h"
#include "players/player_two_camera.h"
#include "players/scopes.h"

#include <cstdint>
#include <cstring>
#include <format>

namespace lss::battle::targeting_two {

namespace {

using platform::Read;

using SysFn       = void (*)(void* system);
using SysDtFn     = void (*)(void* system, float deltaTime);
using SetLockFn   = char (*)(void* system, std::uint8_t on, std::int32_t a3);
using ClearFn     = std::int64_t (*)(void* system, std::int64_t a2, std::int32_t a3);

constexpr int      kPlayer = 1;
constexpr unsigned kScopes = players::scopes::Primary | players::scopes::Operator | players::scopes::Leader |
                             players::scopes::UiSlot | players::scopes::Focus;

// Set once at install.
void**         g_system      = nullptr;  // the battle system global
std::uintptr_t g_slot        = 0;        // EndUISystem[19]
std::uintptr_t g_normalCall  = 0;
std::uintptr_t g_target      = 0;        // BattleSystem_TickTargeting
SysFn          g_preTick     = nullptr;
SetLockFn      g_setLock     = nullptr;
ClearFn        g_clear       = nullptr;
platform::CallRedirect g_pausedCall;
platform::CallRedirect g_normalCallRedirect;

// Game thread.
platform::ByteContext g_fields;        // player 2's copy of the battle system's target fields
void*                 g_fieldsSystem = nullptr;  // the system the copy was defined on
bool                  g_enabled      = true;
bool                  g_placed       = false;
bool                  g_inBattle     = false;
std::uint8_t          g_lastLock     = 0;
std::uint32_t         g_runs         = 0;

void* System() noexcept
{
    void* system = nullptr;
    return Read(g_system, system) ? system : nullptr;
}

std::uint8_t Byte(const void* object, std::uint32_t offset) noexcept
{
    std::uint8_t value = 0;
    Read(static_cast<const std::uint8_t*>(object) + offset, value);
    return value;
}

/// Player 2's copy for this battle system, defined when the system is new (all zero: no lock, no target).
bool FieldsFor(void* system) noexcept
{
    if (system == g_fieldsSystem && g_fields.Defined())
    {
        return true;
    }
    const platform::ByteContext::Range range{reinterpret_cast<std::uintptr_t>(system) + facts::kBattleSysTargetFields,
                                             facts::kBattleSysTargetFieldsBytes, platform::ByteContext::Kind::Exchange};
    if (!g_fields.Define(std::span<const platform::ByteContext::Range>(&range, 1)))
    {
        return false;
    }
    g_fieldsSystem = system;
    g_lastLock     = 0;
    platform::LogDebug(L"targeting 2: player 2's target fields start empty on battle system {:#x}", reinterpret_cast<std::uintptr_t>(system));
    return true;
}

/// Ends player 2's lock and target with the game's own clear, on its copy, and forgets the copy.
void End(const wchar_t* why) noexcept
{
    if (!g_fields.Defined())
    {
        return;
    }
    void* system = System();
    if (system != nullptr && system == g_fieldsSystem)
    {
        platform::ByteScope fields(g_fields);
        if (fields.Held())
        {
            std::int64_t ignored = 0;
            platform::CallGuarded(g_clear, ignored, system, std::int64_t{0}, std::int32_t{0});
        }
    }
    else
    {
        platform::LogDebug(L"targeting 2: the battle system is gone; player 2's target copy dropped ({})", why);
    }
    g_fields.Clear();
    g_fieldsSystem = nullptr;
    g_lastLock     = 0;
    platform::LogDebug(L"targeting 2: player 2's lock and target ended ({})", why);
}

/// BattleSystem_TickTargeting at the game's two calls: the game's run, then player 2's.
void Targeting(void* system, float deltaTime)
{
    reinterpret_cast<SysDtFn>(g_target)(system, deltaTime);
    if (!g_enabled || players::scopes::Member(kPlayer) == nullptr || !FieldsFor(system))
    {
        return;
    }
    {
        players::scopes::Scope                 scope(kPlayer, kScopes);
        players::pad_buttons::ButtonScope      buttons(1);
        players::player_two_camera::StickScope stick(1, true);
        const FieldsScope                      fields;
        if (!scope.Held() || !fields.Held())
        {
            return;
        }
        ++g_runs;
        g_preTick(system);
        reinterpret_cast<SysDtFn>(g_target)(system, deltaTime);
    }
    const std::uint8_t lockTwo = g_fields.Value(0)[facts::kBattleSysLock - facts::kBattleSysTargetFields];
    if (lockTwo != g_lastLock)
    {
        g_lastLock = lockTwo;
        platform::LogDebug(L"targeting 2: player 2's lock {}", lockTwo ? L"on" : L"off");
    }
}

template <class Fn>
bool FindInto(const platform::GameImage& image, const platform::FunctionFact& fact, Fn& out) noexcept
{
    const auto address = platform::FindFunction(image, fact);
    out                = address ? reinterpret_cast<Fn>(*address) : nullptr;
    return out != nullptr;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system     = image.Rebase(facts::kBattleSystemGlobal, sizeof(void*));
    const auto slot       = image.Rebase(facts::kCommandCtrlSlotGlobal, sizeof(void*));
    const auto target     = platform::FindFunction(image, facts::kBattleTickTargeting);
    const auto pausedCall = image.Rebase(facts::kTargetingCallPaused, 5);
    const auto normalCall = image.Rebase(facts::kTargetingCallNormal, 5);
    if (!system || !slot || !target || !pausedCall || !normalCall || !FindInto(image, facts::kBattleTargetPreTick, g_preTick) ||
        !FindInto(image, facts::kBattleSetLockOn, g_setLock) || !FindInto(image, facts::kBattleClearLockAndTarget, g_clear))
    {
        platform::Log(L"targeting 2: an engine check failed; player 2 has no targeting or lock-on");
        return false;
    }
    g_system     = reinterpret_cast<void**>(*system);
    g_slot       = *slot;
    g_target     = *target;
    g_normalCall = *normalCall;
    if (!g_pausedCall.Install(*pausedCall, g_target, reinterpret_cast<std::uintptr_t>(&Targeting)))
    {
        platform::Log(L"targeting 2: the paused-path call could not be redirected; player 2 has no targeting or lock-on");
        return false;
    }
    control::Register({.name  = L"targeting-two",
                       .shape = control::Shape::OnOff,
                       .help  = L"the game's targeting tick runs again for player 2 (battle/targeting_two.h; on)",
                       .set   = [](const control::Value& value) {
                           g_enabled = std::get<bool>(value);
                           platform::Log(L"targeting 2 {}", g_enabled ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"targeting-two.runs", .shape = control::Shape::Integer, .max = INT32_MAX,
                       .help = L"player 2's targeting runs so far",
                       .get  = [] { return control::Value{static_cast<std::int64_t>(g_runs)}; }});
    control::Register({.name = L"targeting-two.lock", .shape = control::Shape::Text,
                       .help = L"the battle system's lock byte (player 1's, live) and player 2's copy",
                       .get  = [] {
                           void* system = System();
                           return control::Value{std::format(L"p1 {} p2 {}", system == nullptr ? -1 : Byte(system, facts::kBattleSysLock),
                                                             g_fields.Defined() ? g_fields.Value(0)[facts::kBattleSysLock - facts::kBattleSysTargetFields] : -1)};
                       }});
    return true;
}

void Remove() noexcept
{
    g_normalCallRedirect.Restore();
    g_pausedCall.Restore();
}

void Tick() noexcept
{
    if (!g_placed && g_normalCall != 0)
    {
        g_placed = true;
        if (g_normalCallRedirect.InstallFromOwnThread(g_normalCall, g_target, reinterpret_cast<std::uintptr_t>(&Targeting)))
        {
            platform::Log(L"targeting 2: installed; the game's targeting tick runs again for player 2");
        }
        else
        {
            platform::Log(L"targeting 2: the normal-path call could not be redirected; player 2's targeting runs only while paused");
        }
    }
    // The battle's end: the game's own controller leaves its battle HUD mode, or the battle system goes.
    std::uint8_t* ctrl = nullptr;
    Read(reinterpret_cast<const void*>(g_slot), ctrl);
    const bool inBattle = ctrl != nullptr && Byte(ctrl, facts::kCommandCtrlBattleHudMode) != 0 && System() != nullptr;
    if (g_inBattle && !inBattle)
    {
        End(L"the battle ended");
    }
    else if (g_fields.Defined() && System() != g_fieldsSystem)
    {
        End(L"the battle system changed");
    }
    g_inBattle = inBattle;
}

void OnLeave() noexcept
{
    End(L"player 2 left");
}

FieldsScope::FieldsScope() noexcept : system_(System())
{
    if (g_fields.Defined() && system_ != nullptr && system_ == g_fieldsSystem)
    {
        lockOne_ = Byte(system_, facts::kBattleSysLock);
        held_    = g_fields.Open();
    }
}

FieldsScope::~FieldsScope()
{
    if (!held_)
    {
        return;
    }
    const std::uint8_t lockTwo  = Byte(system_, facts::kBattleSysLock);
    const std::uint8_t latchTwo = Byte(system_, facts::kBattleSysDummyLatch);
    if (lockTwo != lockOne_)
    {
        // The game-wide Global.LockOn follows the lock's setter; player 1's lock goes back through it while player
        // 2's fields are still in, so the parameter is player 1's again.
        char ignored = 0;
        platform::CallGuarded(g_setLock, ignored, system_, lockOne_, std::int32_t{0});
    }
    g_fields.Close();
    // The setter wrote player 1's lock (and the dummy-lock latch it derives) into the live bytes the copy took back:
    // player 2's own go in again.
    g_fields.Value(0)[facts::kBattleSysLock - facts::kBattleSysTargetFields]       = lockTwo;
    g_fields.Value(0)[facts::kBattleSysDummyLatch - facts::kBattleSysTargetFields] = latchTwo;
}

}  // namespace lss::battle::targeting_two
