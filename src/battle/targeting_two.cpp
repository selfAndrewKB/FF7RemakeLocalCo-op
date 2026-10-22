#include "battle/targeting_two.h"

#include "battle/target_cursor_two.h"
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
#include "players/seats.h"

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace lss::battle::targeting_two {

namespace {

using platform::Read;
using players::kSeatsBuilt;
using players::SeatBuilt;
using players::SeatName;

using SysFn       = void (*)(void* system);
using SysDtFn     = void (*)(void* system, float deltaTime);
using SetLockFn   = char (*)(void* system, std::uint8_t on, std::int32_t a3);
using ClearFn     = std::int64_t (*)(void* system, std::int64_t a2, std::int32_t a3);

constexpr unsigned kScopes = players::scopes::Primary | players::scopes::Operator | players::scopes::Leader |
                             players::scopes::UiSlot | players::scopes::Focus;
constexpr std::uint32_t kLockAt  = facts::kBattleSysLock - facts::kBattleSysTargetFields;        // in the copy
constexpr std::uint32_t kLatchAt = facts::kBattleSysDummyLatch - facts::kBattleSysTargetFields;

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

// Game thread. Index = seat ([0] unused): each seat's copy of the battle system's target fields.
struct Seat
{
    platform::ByteContext fields;
    void*                 system   = nullptr;  // the battle system the copy was defined on
    std::uint8_t          lastLock = 0;
    std::uint32_t         runs     = 0;
};
std::array<Seat, players::scopes::kMaxPlayers> g_seats;
// The cursor (battle/target_cursor_two.h) is one instance, seat 1's: RunSeat's driver call and End's release are
// gated on seat 1 until that module keeps one per seat (plan/seats.md step 3).
static_assert(kSeatsBuilt == 1, "the cursor driver and its release run for seat 1 alone until target_cursor_two is per seat");
bool                                           g_enabled  = true;
bool                                           g_placed   = false;
bool                                           g_inBattle = false;

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

std::uint8_t LockOf(const Seat& s) noexcept
{
    return s.fields.Defined() ? s.fields.Value(0)[kLockAt] : std::uint8_t{0};
}

/// The seat's copy for this battle system, defined when the system is new (all zero: no lock, no target).
bool FieldsFor(int seat, void* system) noexcept
{
    Seat& s = g_seats[seat];
    if (system == s.system && s.fields.Defined())
    {
        return true;
    }
    if (s.fields.Defined())
    {
        // The battle system changed before Tick noticed: the old copy goes without the game's clear (its system is gone).
        platform::LogDebug(L"targeting 2: {}'s target copy redefined on a new battle system; the old one dropped", SeatName(seat));
    }
    const platform::ByteContext::Range range{reinterpret_cast<std::uintptr_t>(system) + facts::kBattleSysTargetFields,
                                             facts::kBattleSysTargetFieldsBytes, platform::ByteContext::Kind::Exchange};
    if (!s.fields.Define(std::span<const platform::ByteContext::Range>(&range, 1)))
    {
        return false;
    }
    s.system   = system;
    s.lastLock = 0;
    platform::LogDebug(L"targeting 2: {}'s target fields start empty on battle system {:#x}", SeatName(seat),
                       reinterpret_cast<std::uintptr_t>(system));
    return true;
}

/// Ends the seat's lock and target with the game's own clear, on its copy, and forgets the copy.
void End(int seat, const wchar_t* why) noexcept
{
    Seat& s = g_seats[seat];
    if (!s.fields.Defined())
    {
        return;
    }
    if (s.fields.IsOpen())
    {
        // A call made for the seat is running with its copy in (no such path today): the copy cannot go now.
        platform::LogDebug(L"targeting 2: {}'s targets' end deferred, its copy is in use ({})", SeatName(seat), why);
        return;
    }
    if (seat == 1)
    {
        target_cursor_two::End();  // the seat's cursor released while its member and the battle system exist
    }
    void* system = System();
    if (system != nullptr && system == s.system)
    {
        platform::ByteScope fields(s.fields);
        if (fields.Held())
        {
            std::int64_t ignored = 0;
            platform::CallGuarded(g_clear, ignored, system, std::int64_t{0}, std::int32_t{0});
        }
    }
    else
    {
        platform::LogDebug(L"targeting 2: the battle system is gone; {}'s target copy dropped ({})", SeatName(seat), why);
    }
    s.fields.Clear();
    s.system   = nullptr;
    s.lastLock = 0;
    platform::LogDebug(L"targeting 2: {}'s lock and target ended ({})", SeatName(seat), why);
}

/// The pre-tick and the targeting tick again for one seat, inside its scopes with its copy in.
void RunSeat(int seat, void* system, float deltaTime) noexcept
{
    Seat& s = g_seats[seat];
    if (players::scopes::Member(seat) == nullptr || !FieldsFor(seat, system))
    {
        return;
    }
    {
        players::scopes::Scope                 scope(seat, kScopes);
        players::pad_buttons::ButtonScope      buttons(static_cast<unsigned>(seat));
        players::player_two_camera::StickScope stick(static_cast<std::uint32_t>(seat), true);
        const FieldsScope                      fields(seat);
        if (!scope.Held() || !fields.Held())
        {
            return;
        }
        ++s.runs;
        g_preTick(system);
        reinterpret_cast<SysDtFn>(g_target)(system, deltaTime);
        // The game's cursor driver follows its targeting call: one cursor, seat 1's (the pin above).
        if (seat == 1)
        {
            target_cursor_two::RunForPlayerTwo(system);
        }
    }
    const std::uint8_t lock = LockOf(s);
    if (lock != s.lastLock)
    {
        s.lastLock = lock;
        platform::LogDebug(L"targeting 2: {}'s lock {}", SeatName(seat), lock ? L"on" : L"off");
    }
}

/// BattleSystem_TickTargeting at the game's two calls: the game's run, then each built seat's.
void Targeting(void* system, float deltaTime)
{
    reinterpret_cast<SysDtFn>(g_target)(system, deltaTime);
    if (!g_enabled)
    {
        return;
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        RunSeat(seat, system, deltaTime);
    }
}

std::uint32_t Runs() noexcept
{
    std::uint32_t runs = 0;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        runs += g_seats[seat].runs;
    }
    return runs;
}

/// Player 1's live lock byte and each seat's copy.
std::wstring LockReading()
{
    void*        system = System();
    std::wstring text   = std::format(L"player 1 {}", system == nullptr ? -1 : Byte(system, facts::kBattleSysLock));
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        text += std::format(L"; {} {}", SeatName(seat), g_seats[seat].fields.Defined() ? LockOf(g_seats[seat]) : -1);
    }
    return text;
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
        platform::Log(L"targeting 2: an engine check failed; no seat has targeting or lock-on");
        return false;
    }
    g_system     = reinterpret_cast<void**>(*system);
    g_slot       = *slot;
    g_target     = *target;
    g_normalCall = *normalCall;
    if (!g_pausedCall.Install(*pausedCall, g_target, reinterpret_cast<std::uintptr_t>(&Targeting)))
    {
        platform::Log(L"targeting 2: the paused-path call could not be redirected; no seat has targeting or lock-on");
        return false;
    }
    control::Register({.name  = L"targeting-two",
                       .shape = control::Shape::OnOff,
                       .help  = L"the game's targeting tick runs again for each seat (battle/targeting_two.h; on)",
                       .set   = [](const control::Value& value) {
                           g_enabled = std::get<bool>(value);
                           platform::Log(L"targeting 2 {}", g_enabled ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"targeting-two.runs", .shape = control::Shape::Integer, .max = INT32_MAX,
                       .help = L"the seats' targeting runs so far, together",
                       .get  = [] { return control::Value{static_cast<std::int64_t>(Runs())}; }});
    control::Register({.name = L"targeting-two.lock", .shape = control::Shape::Text,
                       .help = L"the battle system's lock byte (player 1's, live) and each seat's copy (-1: none)",
                       .get  = [] { return control::Value{LockReading()}; }});
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
            platform::Log(L"targeting 2: installed; the game's targeting tick runs again for each seat");
        }
        else
        {
            platform::Log(L"targeting 2: the normal-path call could not be redirected; the seats' targeting runs only while paused");
        }
    }
    // The battle's end: the game's own controller leaves its battle HUD mode, or the battle system goes.
    std::uint8_t* ctrl = nullptr;
    Read(reinterpret_cast<const void*>(g_slot), ctrl);
    void*      system   = System();
    const bool inBattle = ctrl != nullptr && Byte(ctrl, facts::kCommandCtrlBattleHudMode) != 0 && system != nullptr;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        if (g_inBattle && !inBattle)
        {
            End(seat, L"the battle ended");
        }
        else if (g_seats[seat].fields.Defined() && system != g_seats[seat].system)
        {
            End(seat, L"the battle system changed");
        }
    }
    g_inBattle = inBattle;
}

void OnLeave(int seat) noexcept
{
    if (SeatBuilt(seat))
    {
        End(seat, L"its player left");
    }
}

std::uint8_t Lock(int seat) noexcept
{
    return SeatBuilt(seat) ? LockOf(g_seats[seat]) : std::uint8_t{0};
}

FieldsScope::FieldsScope(int seat) noexcept : seat_(seat), system_(System())
{
    if (!SeatBuilt(seat))
    {
        return;
    }
    Seat& s = g_seats[seat];
    if (s.fields.Defined() && system_ != nullptr && system_ == s.system)
    {
        lockOne_ = Byte(system_, facts::kBattleSysLock);
        held_    = s.fields.Open();
    }
}

FieldsScope::~FieldsScope()
{
    if (!held_)
    {
        return;
    }
    Seat&              s        = g_seats[seat_];
    const std::uint8_t lockSeat = Byte(system_, facts::kBattleSysLock);
    const std::uint8_t latch    = Byte(system_, facts::kBattleSysDummyLatch);
    if (lockSeat != lockOne_)
    {
        // The game-wide Global.LockOn follows the lock's setter; player 1's lock goes back through it while the
        // seat's fields are still in, so the parameter is player 1's again.
        char ignored = 0;
        platform::CallGuarded(g_setLock, ignored, system_, lockOne_, std::int32_t{0});
    }
    s.fields.Close();
    // The setter wrote player 1's lock (and the dummy-lock latch it derives) into the live bytes the copy took back:
    // the seat's own go in again.
    s.fields.Value(0)[kLockAt]  = lockSeat;
    s.fields.Value(0)[kLatchAt] = latch;
}

}  // namespace lss::battle::targeting_two
