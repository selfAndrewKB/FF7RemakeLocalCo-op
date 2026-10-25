#include "world/traversal_action.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/pad_buttons.h"
#include "players/player_two_camera.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/weak_object.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <numbers>
#include <span>
#include <string>

namespace lss::world::traversal_action {

namespace {

using platform::Read;
using platform::ReadPointer;

using CompTickFn   = void (*)(void* component, float dt);
using StartEndFn   = void (*)(void* component, char start, char end);
using LockFn       = std::uint64_t (*)(void* stateManager, std::int32_t mask, std::uint64_t reason);
using QueueLockFn  = void (*)(std::int8_t bit, std::uint64_t reason);
using DeactivateFn = void (*)(std::uint8_t id);
using FlavorLeaveFn = void (*)(void* flavor);
using PostTickFn   = void (*)(void* state, float dt);
using BalanceEndFn = void (*)(void** capture, char reached);
using NearExitFn   = void (*)(void** capture, char end);
using StateCallFn  = void (*)(void* state);
using PhaseFn      = void (*)(void** capture, char phase);
using LadderLeaveFn = void (*)(void* flavor, char keepYaw);

constexpr int           kSeats   = players::scopes::kMaxPlayers;
constexpr std::uint8_t  kFlavors[] = {facts::kFlavorLadder, facts::kFlavorDuct, facts::kFlavorCustomizable};

struct SeatState
{
    void*         member    = nullptr;
    void*         component = nullptr;
    std::uint8_t  action    = 0;  // the component's running action at the last tick
    std::uint32_t actions = 0, camerasLeft = 0, camerasOff = 0, locksSkipped = 0;  // left: the game's eased leave
    std::uint32_t leaderResumed = 0;  // Start/Exit calls run with the seat's leader inside the action start
    std::int8_t   ladderKeepYaw = -1;  // an exit phase noted as the climb ended: its leave's keep-yaw flag (-1 none)
};

// Set at install, then only read.
std::uintptr_t g_fasGlobal = 0;
std::uintptr_t g_root      = 0;
std::uintptr_t g_party     = 0;
DeactivateFn   g_deactivate = nullptr;
FlavorLeaveFn  g_ductLeave  = nullptr;
LadderLeaveFn  g_ladderLeave = nullptr;
std::uintptr_t g_lockSet = 0, g_lockClear = 0, g_queueSet = 0, g_queueClear = 0;
std::uintptr_t g_crawlLockSite = 0, g_crawlUnlockSite = 0, g_squeezeLockSite = 0, g_squeezeUnlockSite = 0;
bool           g_installed = false;

platform::CallRedirect  g_compTick;
platform::EntryRedirect g_startEnd;
platform::CallRedirect  g_crawlLock, g_crawlUnlock, g_squeezeLock, g_squeezeUnlock;
platform::VTableEntry   g_slideDownPostTick;
platform::EntryRedirect g_balanceEnd;
platform::EntryRedirect g_squeezeNearExit;
platform::CallRedirect  g_crawlNearExit;
platform::VTableEntry   g_monkeyBarsStart, g_monkeyBarsExit, g_balanceStart;
platform::CallRedirect  g_monkeyBarsBaseExit;
platform::CallRedirect  g_ladderPhase;

std::atomic<DWORD> g_gameThread{0};
std::atomic<int>   g_ticking{0};       // the seat whose member's component the game thread is ticking now
std::atomic<bool>  g_onGame{false};    // the field-action tick ran on the game thread since the mod moved it
std::atomic<std::uint32_t> g_workerTicks{0};
std::array<std::atomic<void*>, kSeats> g_seatComponents{};  // each seat's member's component, for the animation side
// Written on the game thread, read on any (a state's slot can be called on a worker while the tick is not moved).
std::atomic<int>   g_pausedSeat{0};    // the seat whose action start runs now with player 1's leader
std::atomic<int>   g_resumedSeat{0};   // the seat whose Start or Exit runs now with its own leader again
std::array<std::atomic<std::uint8_t>, kSeats> g_ductNearEnd{};  // the action (squeeze, crawl) whose near-exit call ran for a seat's member
std::array<std::atomic<void*>, kSeats>        g_ladderExit{};       // a seat's member's ladder state that reached an exit phase
std::array<std::atomic<std::uint8_t>, kSeats> g_ladderExitPhase{};  // that phase (written before the state)
std::atomic<int> g_ladderThread{0};  // the ladder's phase call seen on: 1 the game thread, 2 another (logged once)
bool             g_ladderThreadTold = false;
std::atomic<std::int64_t>  g_workerTime{0};  // performance-counter ticks spent inside component ticks on workers

// Game thread.
std::array<SeatState, kSeats> g_seats{};
std::uintptr_t g_fas       = 0;  // the FieldActionSystem whose tick the mod moved
bool           g_moved     = false;
bool           g_locksPlaced = false;
std::atomic<bool> g_locksOk{false};  // all four lock calls redirected: a seat's member may tick in its scopes (read on any thread)
bool           g_crawlLocking = false;  // inside player 1's crawl lock call
std::atomic<bool> g_squeezeQueued{false};  // player 1's squeeze lock queued and not yet unqueued (any thread)
bool           g_neverMovable = false;  // a FieldActionSystem whose tick never ran on any thread: logged once
std::uint32_t  g_gameTicks = 0;
std::int64_t   g_gameTime  = 0;  // performance-counter ticks spent inside component ticks on the game thread

/// The frame cost of the component ticks, per 5 s window (game thread): the user's condition for the move (2026-10-24).
struct CostWindow
{
    std::int64_t began = 0, game = 0, worker = 0;
    std::uint32_t frames = 0;
    double gameMs = -1, workerMs = -1;  // per frame over the last full window; negative until one ends
};
CostWindow g_cost;

std::int64_t Now() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

/// Performance-counter ticks per second.
std::int64_t Frequency() noexcept
{
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}

/// Adds the time from its construction to its end to a game-thread total.
struct TimeSpent
{
    std::int64_t& into;
    std::int64_t  began = Now();
    explicit TimeSpent(std::int64_t& total) noexcept : into(total) {}
    ~TimeSpent() { into += Now() - began; }
    TimeSpent(const TimeSpent&)            = delete;
    TimeSpent& operator=(const TimeSpent&) = delete;
};

/// A byte of the game's held at a value for one call (put back after).
struct KeptByte
{
    std::uint8_t* at    = nullptr;
    std::uint8_t  value = 0;
    bool          kept  = false;
    explicit KeptByte(std::uint8_t* address, std::uint8_t replacement) noexcept : at(address)
    {
        kept = address != nullptr && Read(address, value) && platform::WriteGuarded(address, &replacement, 1);
    }
    ~KeptByte()
    {
        if (kept)
        {
            platform::WriteGuarded(at, &value, 1);
        }
    }
    KeptByte(const KeptByte&)            = delete;
    KeptByte& operator=(const KeptByte&) = delete;
};

bool OnGameThreadNow() noexcept
{
    return GetCurrentThreadId() == g_gameThread.load(std::memory_order_relaxed);
}

int SeatOfComponent(const void* component) noexcept;

/// Runs a seat's member's field-action code as the seat's (game thread): its Leader, Primary and Operator scopes, its
/// pad's stick and buttons, player 1's camera-cut stick latch cleared. Without the scopes it runs as the game's member.
template <class Call>
void RunAsSeat(int seat, Call&& call)
{
    if (g_ticking.load(std::memory_order_relaxed) != 0)
    {
        platform::LogDebug(L"traversal: {}'s field-action call came inside another seat's; run as it is", players::SeatName(seat));
        call();  // the outer call holds the scopes (they do not nest)
        return;
    }
    g_ticking.store(seat, std::memory_order_relaxed);
    {
        const players::scopes::Scope scope(seat, players::scopes::Leader | players::scopes::Primary | players::scopes::Operator);
        if (scope.Held())
        {
            const players::player_two_camera::StickScope stick(static_cast<std::uint32_t>(seat), true);
            const players::pad_buttons::ButtonScope      buttons(static_cast<unsigned>(seat));
            auto*                                        root = static_cast<std::uint8_t*>(ReadPointer(reinterpret_cast<const void*>(g_root), 0));
            const KeptByte latch(root != nullptr ? root + facts::kRootStickLatch : nullptr, 0);  // player 1's camera-cut latch
            call();
        }
        else
        {
            call();  // a seat without its scopes: the game's member version
        }
    }
    g_ticking.store(0, std::memory_order_relaxed);
}

int SeatOfComponent(const void* component) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (component != nullptr && component == g_seats[seat].component)
        {
            return seat;
        }
    }
    return 0;
}

void CompTick(void* component, float dt)
{
    const auto original = reinterpret_cast<CompTickFn>(g_compTick.Original());
    if (!OnGameThreadNow())
    {
        const std::int64_t began = Now();
        g_workerTicks.fetch_add(1, std::memory_order_relaxed);
        original(component, dt);
        g_workerTime.fetch_add(Now() - began, std::memory_order_relaxed);
        return;
    }
    ++g_gameTicks;
    const TimeSpent spent(g_gameTime);
    if (g_moved)
    {
        g_onGame.store(true, std::memory_order_relaxed);
    }
    const int seat = g_locksOk.load(std::memory_order_relaxed) ? SeatOfComponent(component) : 0;  // scoped only once a seat's action cannot take the lock
    if (seat == 0)
    {
        original(component, dt);
        return;
    }
    RunAsSeat(seat, [&] { original(component, dt); });
}

/// A seat's member's component (any thread), or 0.
int SeatOfComponentAnyThread(const void* component) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (component != nullptr && component == g_seatComponents[seat].load(std::memory_order_relaxed))
        {
            return seat;
        }
    }
    return 0;
}

/// The balance beam's end delegate (animation side): for a seat's member, its stored camera is kept out of the
/// delegate's test of player 1's camera, so the state's exit (inside the action start, on the seat's operator; it has no
/// leader test) eases the seat's camera out.
void BalanceEnd(void** capture, char reached)
{
    const auto original = reinterpret_cast<BalanceEndFn>(g_balanceEnd.Original());
    auto*      state    = capture != nullptr ? static_cast<std::uint8_t*>(ReadPointer(capture, 0)) : nullptr;
    const int  seat     = state != nullptr && Scoping() ? SeatOfComponentAnyThread(ReadPointer(state, facts::kStateComponent)) : 0;
    void*      stored   = seat != 0 ? ReadPointer(state, facts::kBalanceStoredFlavor) : nullptr;
    if (stored == nullptr)
    {
        original(capture, reached);
        return;
    }
    void* none = nullptr;
    const bool hidden = platform::WriteGuarded(state + facts::kBalanceStoredFlavor, &none, sizeof(none));
    original(capture, reached);
    if (hidden && !platform::WriteGuarded(state + facts::kBalanceStoredFlavor, &stored, sizeof(stored)))
    {
        platform::LogDebug(L"traversal: {}'s beam camera could not be kept for its exit; it is switched off after the beam",
                           players::SeatName(seat));
    }
}

/// After a near-exit delegate ran (animation side): for a seat's member's state, the seat's duct leave is started on the
/// game thread next tick.
void NoteNearEnd(void* const* capture, std::uint8_t action) noexcept
{
    const void* state = capture != nullptr ? ReadPointer(capture, 0) : nullptr;
    if (const int seat = state != nullptr && Scoping() ? SeatOfComponentAnyThread(ReadPointer(state, facts::kStateComponent)) : 0; seat != 0)
    {
        g_ductNearEnd[seat].store(action, std::memory_order_relaxed);
    }
}

/// The squeeze's and the crawl's near-exit delegates (animation side): the game starts the leader's duct camera leave
/// there, which the delegates' leader-type test skips for a seat's member.
void SqueezeNearExit(void** capture, char end)
{
    reinterpret_cast<NearExitFn>(g_squeezeNearExit.Original())(capture, end);
    NoteNearEnd(capture, facts::kFieldActionSqueeze);
}

void CrawlNearExit(void** capture, char end)
{
    reinterpret_cast<NearExitFn>(g_crawlNearExit.Original())(capture, end);
    NoteNearEnd(capture, facts::kFieldActionCrawl);
}

/// The ladder behaviour's phase delegate (animation side): the game asks the ladder camera to leave on an exit phase
/// for the party leader's type only. For a seat's member's exit phase the call is noted, and the game thread runs the
/// same delegate in the seat's scopes the next tick (the seat's leader type, the seat's camera).
void LadderPhase(void** capture, char phase)
{
    reinterpret_cast<PhaseFn>(g_ladderPhase.Original())(capture, phase);
    const auto reached = static_cast<std::uint8_t>(phase);
    if (reached != facts::kLadderTopExit && reached != facts::kLadderBottomExit && reached != facts::kLadderSlideLanding &&
        reached != facts::kLadderJumpOff)
    {
        return;
    }
    void* state = capture != nullptr ? ReadPointer(capture, 0) : nullptr;
    if (const int seat = state != nullptr && Scoping() ? SeatOfComponentAnyThread(ReadPointer(state, facts::kStateComponent)) : 0; seat != 0)
    {
        g_ladderThread.store(OnGameThreadNow() ? 1 : 2, std::memory_order_relaxed);  // research: which thread the animation side is
        g_ladderExitPhase[seat].store(reached, std::memory_order_relaxed);
        g_ladderExit[seat].store(state, std::memory_order_release);
    }
}

/// The slide-down's post-tick (its side steer, outside the component tick): a seat's member's runs as its component
/// tick does, so pad 2 or 3 steers it.
void SlideDownPostTick(void* state, float dt)
{
    const auto original = reinterpret_cast<PostTickFn>(g_slideDownPostTick.Original());
    const int  seat     = OnGameThreadNow() && g_locksOk.load(std::memory_order_relaxed) ? SeatOfComponent(ReadPointer(state, facts::kStateComponent)) : 0;
    if (seat == 0)
    {
        original(state, dt);
        return;
    }
    RunAsSeat(seat, [&] { original(state, dt); });
}

/// The ticking seat whose member owns `component`, on the game thread; 0 otherwise.
int TickingSeatOf(const void* component) noexcept
{
    const int seat = g_ticking.load(std::memory_order_relaxed);
    return seat != 0 && OnGameThreadNow() && component == g_seats[seat].component ? seat : 0;
}

/// A seat number held in one of the markers above for one call, the outer value put back after (nesting).
class SeatMark
{
public:
    SeatMark(std::atomic<int>& mark, int seat) noexcept : mark_(mark), outer_(mark.exchange(seat, std::memory_order_relaxed)) {}
    ~SeatMark() { mark_.store(outer_, std::memory_order_relaxed); }
    SeatMark(const SeatMark&)            = delete;
    SeatMark& operator=(const SeatMark&) = delete;

private:
    std::atomic<int>& mark_;
    int               outer_;
};

void StartEnd(void* component, char start, char end)
{
    const auto original = reinterpret_cast<StartEndFn>(g_startEnd.Original());
    if (const int seat = TickingSeatOf(component))
    {
        const players::scopes::LeaderPause pause(seat);  // player 1's leader: the seat's action takes no global switch
        const SeatMark                     paused(g_pausedSeat, pause.Paused() ? seat : 0);
        original(component, start, end);
        return;
    }
    original(component, start, end);
}

/// A state's own Start or Exit (`slot` holds the game's). Called by the action start for a seat's member, while the mod
/// holds player 1's leader there, it runs with the seat's leader again for this one call: its camera code (the leader
/// type's) then answers for the seat, on the seat's operator. Any other call is the game's.
void WithSeatLeader(const platform::VTableEntry& slot, void* state, const wchar_t* what)
{
    const auto original = reinterpret_cast<StateCallFn>(slot.Original());
    const int  seat     = g_pausedSeat.load(std::memory_order_relaxed);
    if (seat == 0 || !OnGameThreadNow() || ReadPointer(state, facts::kStateComponent) != g_seats[seat].component)
    {
        original(state);
        return;
    }
    const players::scopes::Scope leader(seat, players::scopes::Leader);
    if (!leader.Held())
    {
        original(state);  // player 1's leader, as before: no camera for the seat
        return;
    }
    ++g_seats[seat].leaderResumed;
    platform::LogDebug(L"traversal: {}'s {} runs with its own member as the leader (its camera)", players::SeatName(seat), what);
    const SeatMark resumed(g_resumedSeat, seat);
    original(state);
}

/// The monkey bars' Exit's first call, the base exit: its end signals choose the leader's set or the member's by the
/// leader type, so inside a resumed Exit it runs with player 1's leader again, as every seat's action end does
/// (a level script listening for the leader's end stays player 1's).
void MonkeyBarsBaseExit(void* state)
{
    const auto original = reinterpret_cast<StateCallFn>(g_monkeyBarsBaseExit.Original());
    const int  seat     = g_resumedSeat.load(std::memory_order_relaxed);
    if (seat == 0 || !OnGameThreadNow() || ReadPointer(state, facts::kStateComponent) != g_seats[seat].component)
    {
        original(state);
        return;
    }
    const players::scopes::LeaderPause pause(seat);
    original(state);
}

void MonkeyBarsStart(void* state) { WithSeatLeader(g_monkeyBarsStart, state, L"monkey-bars start"); }
void MonkeyBarsExit(void* state) { WithSeatLeader(g_monkeyBarsExit, state, L"monkey-bars exit"); }
void BalanceStart(void* state) { WithSeatLeader(g_balanceStart, state, L"balance-beam start"); }

/// Points a state's vtable slot at `thunk` if it holds the function the research read. False (nothing changed) otherwise.
bool ReplaceStateSlot(const platform::GameImage& image, std::uintptr_t slotFact, const platform::FunctionFact& target,
                      platform::VTableEntry& entry, StateCallFn thunk) noexcept
{
    const auto slot = image.Rebase(slotFact, 8);
    const auto fn   = platform::FindFunction(image, target);
    return slot && fn && entry.Replace(*slot, *fn, reinterpret_cast<std::uintptr_t>(thunk));
}

/// A crawl's or squeeze's lock call: skipped while a seat's member's component ticks (game thread).
bool SkipLock() noexcept
{
    const int seat = g_ticking.load(std::memory_order_relaxed);
    if (seat == 0 || !OnGameThreadNow())
    {
        return false;
    }
    ++g_seats[seat].locksSkipped;
    platform::LogDebug(L"traversal: {}'s crawl or squeeze takes no field lock (it would hand the party leader to its AI)", players::SeatName(seat));
    return true;
}

std::uint64_t CrawlLock(void* stateManager, std::int32_t mask, std::uint64_t reason)
{
    if (SkipLock())
    {
        return 0;
    }
    const bool mine = OnGameThreadNow();
    g_crawlLocking  = mine;  // player 1's crawl: the lock's hand of the leader is player 1's alone
    const auto result = reinterpret_cast<LockFn>(g_crawlLock.Original())(stateManager, mask, reason);
    g_crawlLocking  = false;
    return result;
}

std::uint64_t CrawlUnlock(void* stateManager, std::int32_t mask, std::uint64_t reason)
{
    return SkipLock() ? 0 : reinterpret_cast<LockFn>(g_crawlUnlock.Original())(stateManager, mask, reason);
}

void SqueezeLock(std::int8_t bit, std::uint64_t reason)
{
    if (!SkipLock())
    {
        if (OnGameThreadNow())
        {
            g_squeezeQueued.store(true, std::memory_order_relaxed);
        }
        reinterpret_cast<QueueLockFn>(g_squeezeLock.Original())(bit, reason);
    }
}

void SqueezeUnlock(std::int8_t bit, std::uint64_t reason)
{
    if (!SkipLock())
    {
        g_squeezeQueued.store(false, std::memory_order_relaxed);
        reinterpret_cast<QueueLockFn>(g_squeezeUnlock.Original())(bit, reason);
    }
}

const wchar_t* ActionName(std::uint8_t action) noexcept
{
    switch (action)
    {
        case facts::kFieldActionLadder: return L"ladder";
        case facts::kFieldActionCrawl: return L"crawl";
        case facts::kFieldActionSqueeze: return L"squeeze";
        case facts::kFieldActionMonkeyBars: return L"monkey bars";
        case facts::kFieldActionJump: return L"jump";
        case facts::kFieldActionClimb: return L"climb";
        case facts::kFieldActionVault: return L"vault";
        case facts::kFieldActionRubble: return L"rubble step-over";
        case facts::kFieldActionDuck: return L"duck-under";
        case facts::kFieldActionStep: return L"step";
        case facts::kFieldActionSlide: return L"slide";
        case facts::kFieldActionSlideDown: return L"slide-down";
        case facts::kFieldActionFenceGap: return L"fence gap";
        case facts::kFieldActionBalance: return L"balance beam";
        case facts::kFieldActionFan: return L"fan";
        case facts::kFieldActionCounterGate: return L"counter gate";
        case facts::kFieldActionRope: return L"lifting rope";
        default: return L"field action";
    }
}

/// Camera flavor 20 is on its way out (its current phase is the leaving one).
bool Leaving(const void* flavor) noexcept
{
    const void*   phase = ReadPointer(flavor, facts::kFlavorCurrentPhase);
    std::uint32_t kind  = 0;
    return phase != nullptr && Read(phase, kind) && kind == facts::kFlavorPhaseLeaving;
}

/// The duct flavor is leaving already: its current or pending phase (a leave started this tick) is the out or return one.
bool DuctLeaving(const void* flavor) noexcept
{
    for (const std::uint32_t at : {facts::kDuctPhase, facts::kDuctPendingPhase})
    {
        const void*   phase = ReadPointer(flavor, at);
        std::uint32_t kind  = 0;
        if (phase != nullptr && Read(phase, kind) && (kind == facts::kDuctPhaseOut || kind == facts::kDuctPhaseReturn))
        {
            return true;
        }
    }
    return false;
}

/// A seat's member's squeeze or crawl came near its end: the seat's duct camera starts the game's own leave there, as
/// player 1's does from the same delegate (else a squeeze's hold turns it to the member's facing until the end). Each
/// delegate's own condition: the squeeze's leaves whenever the operator has the flavor, the crawl's only once it began.
void DuctLeaveNearEnd(int seat, std::uint8_t action) noexcept
{
    const void*  op     = players::player_two_camera::Operator(seat);
    void*        flavor = op != nullptr ? ReadPointer(op, facts::kOperatorFlavors + 8u * facts::kFlavorDuct) : nullptr;
    std::uint8_t begun  = 0;
    if (flavor == nullptr || g_ductLeave == nullptr ||
        (action == facts::kFieldActionCrawl && (!Read(static_cast<const std::uint8_t*>(flavor) + facts::kDuctBegun, begun) || begun == 0)))
    {
        return;
    }
    if (platform::CallGuarded(g_ductLeave, flavor))
    {
        ++g_seats[seat].camerasLeft;
        platform::LogDebug(L"traversal: {}'s {} camera eases out near the exit, as player 1's does", players::SeatName(seat),
                           ActionName(action));
    }
}

/// The ladder camera is leaving already: its current or pending phase (a leave asked for this tick) is the Leave.
bool LadderLeaving(const void* flavor) noexcept
{
    for (const std::uint32_t at : {facts::kLadderPhaseCurrent, facts::kLadderPhasePending})
    {
        const void*  entry = ReadPointer(flavor, at);
        std::int32_t kind  = -1;
        if (entry != nullptr && Read(entry, kind) && kind == static_cast<std::int32_t>(facts::kLadderPhaseLeave))
        {
            return true;
        }
    }
    return false;
}

/// A seat's member's ladder reached an exit phase (noted on the animation side): the game's own phase delegate again,
/// in the seat's Leader and Operator scopes, so its gate (the leader type, no cutscene) passes for the seat and its leave
/// request reaches the seat's ladder camera, one frame after player 1's would. Not if the climb already ended (the
/// after-action switch-off eases it then) or the camera is leaving already.
void LadderLeaveAtExit(int seat, void* state, std::uint8_t phase) noexcept
{
    const auto*  component = static_cast<const std::uint8_t*>(g_seats[seat].component);
    std::uint8_t action    = 0;
    const void*  op        = players::player_two_camera::Operator(seat);
    void*        flavor    = op != nullptr ? ReadPointer(op, facts::kOperatorFlavors + 8u * facts::kFlavorLadder) : nullptr;
    const bool   climbing  = component != nullptr && Read(component + facts::kFieldActionState, action) &&
                          action == facts::kFieldActionLadder &&
                          ReadPointer(component, facts::kFieldActionStates + 8u * facts::kFieldActionLadder) == state;
    if (!climbing)
    {
        // The climb ended in the same tick: the after-action leave takes the exit's flag (no end-yaw turn at the top).
        g_seats[seat].ladderKeepYaw = phase == facts::kLadderTopExit || phase == facts::kLadderJumpOff ? 1 : 0;
        return;
    }
    if (flavor == nullptr || LadderLeaving(flavor))
    {
        return;
    }
    const players::scopes::Scope scope(seat, players::scopes::Leader | players::scopes::Operator);
    void*                        capture = state;
    if (scope.Held() &&
        platform::CallGuarded(reinterpret_cast<PhaseFn>(g_ladderPhase.Original()), &capture, static_cast<char>(phase)) &&
        LadderLeaving(flavor))
    {
        ++g_seats[seat].camerasLeft;
        platform::LogDebug(L"traversal: {}'s ladder camera eases out at the exit, as player 1's does", players::SeatName(seat));
    }
}

/// After a seat's action ends: the camera flavors its exit left on the seat's operator, switched off by the game.
void CamerasOffAfter(int seat, std::uint8_t ended) noexcept
{
    void* op = players::player_two_camera::Operator(seat);
    if (op == nullptr || g_deactivate == nullptr)
    {
        return;
    }
    for (const std::uint8_t id : kFlavors)
    {
        void*        flavor = ReadPointer(op, facts::kOperatorFlavors + 8u * id);
        std::uint8_t active = 0;
        if (flavor == nullptr || !Read(static_cast<const std::uint8_t*>(flavor) + facts::kFlavorActive, active) || active == 0)
        {
            continue;
        }
        if (id == facts::kFlavorCustomizable)
        {
            if (ended != facts::kFieldActionMonkeyBars && ended != facts::kFieldActionBalance)
            {
                continue;  // not this action's (a script's or a scene's camera on the seat's operator)
            }
            if (Leaving(flavor))
            {
                // Already leaving (usually the state's own exit started it): the game finishes it.
                platform::LogDebug(L"traversal: {}'s {} camera eases out by the game's own exit", players::SeatName(seat),
                                   ActionName(ended));
                continue;
            }
        }
        if (id == facts::kFlavorLadder)
        {
            const char keepYaw             = g_seats[seat].ladderKeepYaw > 0 ? 1 : 0;
            g_seats[seat].ladderKeepYaw = -1;
            if (LadderLeaving(flavor))
            {
                platform::LogDebug(L"traversal: {}'s ladder camera eases out by its exit's leave", players::SeatName(seat));
                continue;
            }
            // The game's own leave (eased): the climb ended before an exit phase, or the exit's leave did not run.
            if (g_ladderLeave != nullptr && platform::CallGuarded(g_ladderLeave, flavor, keepYaw))
            {
                ++g_seats[seat].camerasLeft;
                platform::LogDebug(L"traversal: {}'s ladder camera eases out after the action", players::SeatName(seat));
                continue;
            }
        }
        if (id == facts::kFlavorDuct && g_ductLeave != nullptr)
        {
            if (DuctLeaving(flavor))
            {
                continue;  // its leave started near the exit already
            }
            // The game's own leave (eases back to the follow camera, as player 1's squeeze does): a plain switch-off jumped.
            if (platform::CallGuarded(g_ductLeave, flavor))
            {
                ++g_seats[seat].camerasLeft;
                platform::LogDebug(L"traversal: {}'s duct camera eases out after the action (its exit runs on the animation side, "
                                   L"which tests player 1 only)", players::SeatName(seat));
            }
            continue;
        }
        const players::scopes::Scope scope(seat, players::scopes::Operator);
        if (scope.Held() && platform::CallGuarded(g_deactivate, id))
        {
            ++g_seats[seat].camerasOff;
            platform::LogDebug(L"traversal: {}'s camera {} switched off after the action (its exit runs on the animation side, which "
                               L"tests player 1 only)", players::SeatName(seat), id);
        }
    }
}

void PlaceLocks() noexcept
{
    if (g_locksPlaced || !g_onGame.load(std::memory_order_relaxed))
    {
        return;
    }
    g_locksPlaced = true;  // tried once: a failure is logged and the locks stay the game's
    // These sites run only from the component tick, now on this thread; three cross a 16-byte block.
    const bool placed = g_crawlLock.InstallFromOwnThread(g_crawlLockSite, g_lockSet, reinterpret_cast<std::uintptr_t>(&CrawlLock)) &&
                        g_crawlUnlock.InstallFromOwnThread(g_crawlUnlockSite, g_lockClear, reinterpret_cast<std::uintptr_t>(&CrawlUnlock)) &&
                        g_squeezeLock.InstallFromOwnThread(g_squeezeLockSite, g_queueSet, reinterpret_cast<std::uintptr_t>(&SqueezeLock)) &&
                        g_squeezeUnlock.InstallFromOwnThread(g_squeezeUnlockSite, g_queueClear, reinterpret_cast<std::uintptr_t>(&SqueezeUnlock));
    if (placed)
    {
        g_locksOk.store(true, std::memory_order_relaxed);
        platform::Log(L"traversal: a further player's crawl or squeeze no longer takes the field lock for everyone");
    }
    else
    {
        g_squeezeUnlock.Restore();
        g_squeezeLock.Restore();
        g_crawlUnlock.Restore();
        g_crawlLock.Restore();
        platform::LogWarning(L"traversal: the crawl and squeeze lock calls could not be redirected; further players' members take "
                             L"walk-up spots by themselves, with no pad and no camera of their own");
    }
}

/// Moves the field-action tick to the game thread while a further player is in, and back.
void Place(bool anyIn) noexcept
{
    auto*        fas   = static_cast<std::uint8_t*>(ReadPointer(reinterpret_cast<const void*>(g_fasGlobal), 0));
    std::uint8_t flags = 0;
    if (fas == nullptr || !Read(fas + facts::kFieldActionTickFlags, flags))
    {
        return;
    }
    const auto here = reinterpret_cast<std::uintptr_t>(fas);
    if (anyIn && (flags & facts::kTickRunOnAnyThread) != 0)
    {
        g_onGame.store(false, std::memory_order_relaxed);  // a new FieldActionSystem: seen on the game thread again first
        g_squeezeQueued.store(false, std::memory_order_relaxed);
        const std::uint8_t cleared = flags & static_cast<std::uint8_t>(~facts::kTickRunOnAnyThread);
        if (platform::WriteGuarded(fas + facts::kFieldActionTickFlags, &cleared, 1))
        {
            g_fas   = here;
            g_moved = true;
            platform::Log(L"traversal: the field actions tick on the game thread while further players are in (their members' "
                          L"ladders and squeezes take their own pads and cameras)");
        }
    }
    else if (anyIn && !g_moved && !g_neverMovable)
    {
        g_neverMovable = true;
        platform::LogWarning(L"traversal: this field's actions never tick on any thread; further players' members take walk-up spots "
                             L"as the game's members do");
    }
    else if (!anyIn && g_moved && here != g_fas)
    {
        g_moved = false;  // the field the mod moved is gone; the new one was never moved
        g_onGame.store(false, std::memory_order_relaxed);
    }
    else if (!anyIn && g_moved && here == g_fas && (flags & facts::kTickRunOnAnyThread) == 0)
    {
        const std::uint8_t set = flags | facts::kTickRunOnAnyThread;
        if (platform::WriteGuarded(fas + facts::kFieldActionTickFlags, &set, 1))
        {
            g_moved = false;
            g_onGame.store(false, std::memory_order_relaxed);
            platform::Log(L"traversal: the field actions tick on any thread again (no further player)");
        }
    }
}

/// Player 1's leader's character and its field-action component (null if unread).
const std::uint8_t* PlayerOneLeader(const std::uint8_t** component) noexcept
{
    const auto*  manager = static_cast<const std::uint8_t*>(ReadPointer(reinterpret_cast<const void*>(g_party), 0));
    std::int32_t pair[2]{-1, 0};
    if (manager == nullptr || !platform::CopyGuarded(pair, manager + facts::kPartyLeaderWeak, sizeof(pair)))
    {
        return nullptr;
    }
    const auto* leader = static_cast<const std::uint8_t*>(players::weak_object::ObjectOf(pair));
    *component         = leader != nullptr ? static_cast<const std::uint8_t*>(ReadPointer(leader, facts::kCharacterFieldAction)) : nullptr;
    return leader;
}

/// Player 1's leader is running `action` now.
bool PlayerOneIn(std::uint8_t action) noexcept
{
    const std::uint8_t* comp  = nullptr;
    std::uint8_t        state = 0;
    return PlayerOneLeader(&comp) != nullptr && comp != nullptr && Read(comp + facts::kFieldActionState, state) && state == action;
}

/// Closes a 5 s window of the component ticks' cost (game thread, each engine tick).
void CountCost() noexcept
{
    const std::int64_t frequency = Frequency();
    const std::int64_t now       = Now();
    if (g_cost.began == 0)
    {
        g_cost = CostWindow{.began = now, .game = g_gameTime, .worker = g_workerTime.load(std::memory_order_relaxed)};
        return;
    }
    ++g_cost.frames;
    if (now - g_cost.began < 5 * frequency)
    {
        return;
    }
    const std::int64_t worker = g_workerTime.load(std::memory_order_relaxed);
    const double       toMs   = 1000.0 / static_cast<double>(frequency) / static_cast<double>(g_cost.frames);
    g_cost.gameMs             = static_cast<double>(g_gameTime - g_cost.game) * toMs;
    g_cost.workerMs           = static_cast<double>(worker - g_cost.worker) * toMs;
    g_cost.began = now, g_cost.game = g_gameTime, g_cost.worker = worker, g_cost.frames = 0;
}

/// Dev watch (research/RE-traversal-duct-camera-two.md): each player's squeeze camera (flavor 17 on that player's operator),
/// read at the squeeze's start and through its first 3 s, so player 1's look and a seat's can be compared (the user,
/// 2026-10-25: player 2's camera zooms in, then turns 90 degrees smoothly; player 1's only zooms). The squeezer's facing
/// is the anchor the camera's hold fades to. Read only; Debug.
std::array<std::int64_t, kSeats> g_squeezeStarted{};  // 0 while that player is not squeezing
std::array<int, kSeats>          g_squeezeSamples{};

void LogSqueezeCamera(int player, const void* member, const void* component, const void* op, double seconds) noexcept
{
    const auto* flavor = static_cast<const std::uint8_t*>(op != nullptr ? ReadPointer(op, facts::kOperatorFlavors + 8u * facts::kFlavorDuct)
                                                                       : nullptr);
    const auto* state  = static_cast<const std::uint8_t*>(
        ReadPointer(component, facts::kFieldActionStates + 8u * facts::kFieldActionSqueeze));
    const auto real = [](const void* base, std::uint32_t offset) {
        float value = 0;
        return base != nullptr && Read(static_cast<const std::uint8_t*>(base) + offset, value) ? value
                                                                                                 : std::numeric_limits<float>::quiet_NaN();
    };
    const auto byte = [](const std::uint8_t* base, std::uint32_t offset) {
        std::uint8_t value = 0;
        return base != nullptr && Read(base + offset, value) ? static_cast<int>(value) : -1;
    };
    std::int32_t phase = -1;
    const void*  entry = flavor != nullptr ? ReadPointer(flavor, facts::kDuctPhase) : nullptr;
    if (entry == nullptr || !Read(entry, phase))
    {
        phase = -1;
    }
    std::int32_t side = -1;
    if (state != nullptr)
    {
        Read(state + facts::kSqueezeSide, side);
    }
    const bool follows = op != nullptr && ReadPointer(op, facts::kOperatorOwnerChara) == member;
    float facing = std::numeric_limits<float>::quiet_NaN();
    std::array<float, 4> q{};  // x, y, z, w
    if (const void* root = member != nullptr ? ReadPointer(member, facts::kActorRootComponent) : nullptr;
        root != nullptr && platform::CopyGuarded(q.data(), static_cast<const std::uint8_t*>(root) + facts::kComponentWorldRotation, sizeof(q)))
    {
        facing = static_cast<float>(std::atan2(2.0 * (q[3] * q[2] + q[0] * q[1]), 1.0 - 2.0 * (q[1] * q[1] + q[2] * q[2])) * 180.0 * std::numbers::inv_pi);
    }
    platform::LogDebug(L"squeeze camera: {} at {:.2f} s: on {} begun {}, in time {:.2f} s, phase {} timer {:.2f}, seeded {} (asked {}) yaw "
                       L"{:.1f} anchor {:.2f}; yaw from {:.1f} to {:.1f} (turn {:.1f}), shown {:.1f}, weight {:.2f}; operator yaw {:.1f}, "
                       L"follows the squeezer {}, its facing {:.1f}; squeeze from a point {}, side {}, end yaw {:.1f}",
                       players::SeatName(player), seconds, byte(flavor, facts::kFlavorActive), byte(flavor, facts::kDuctBegun),
                       real(flavor, facts::kDuctInTime), phase, real(flavor, facts::kDuctPhaseTimer), byte(flavor, facts::kDuctSeeded),
                       byte(flavor, facts::kDuctSeedRequest), real(flavor, facts::kDuctSeedYaw), real(flavor, facts::kDuctAnchorWeight),
                       real(flavor, facts::kDuctStartYaw), real(flavor, facts::kDuctTargetYaw), real(flavor, facts::kDuctYawDelta),
                       real(flavor, facts::kDuctOutputYaw), real(flavor, facts::kDuctWeight), real(op, facts::kOperatorPoseYaw),
                       follows ? L"yes" : L"no", facing, byte(state, facts::kSqueezeFromPoint), side, real(state, facts::kSqueezeEndYaw));
}

/// Game thread, each engine tick.
void WatchSqueezeCameras() noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    constexpr double kSampleAt[] = {0.0, 0.1, 0.3, 0.6, 1.0, 1.5, 2.0, 3.0};
    const std::int64_t now = Now();
    for (int player = 0; player <= players::kSeatsBuilt; ++player)
    {
        const void* member    = nullptr;
        const void* component = nullptr;
        const void* op        = nullptr;
        if (player == 0)
        {
            const std::uint8_t* comp = nullptr;
            member    = PlayerOneLeader(&comp);
            component = comp;
            op        = players::player_two_camera::OperatorOne();
        }
        else
        {
            member    = g_seats[player].member;
            component = g_seats[player].component;
            op        = players::player_two_camera::Operator(player);
        }
        std::uint8_t action = 0;
        if (component == nullptr || !Read(static_cast<const std::uint8_t*>(component) + facts::kFieldActionState, action) ||
            action != facts::kFieldActionSqueeze)
        {
            g_squeezeStarted[player] = 0;
            continue;
        }
        if (g_squeezeStarted[player] == 0)
        {
            g_squeezeStarted[player] = now;
            g_squeezeSamples[player] = 0;
        }
        const double seconds = static_cast<double>(now - g_squeezeStarted[player]) / static_cast<double>(Frequency());
        int&         taken   = g_squeezeSamples[player];
        if (taken < static_cast<int>(std::size(kSampleAt)) && seconds >= kSampleAt[taken])
        {
            LogSqueezeCamera(player, member, component, op, seconds);
            while (taken < static_cast<int>(std::size(kSampleAt)) && seconds >= kSampleAt[taken])
            {
                ++taken;
            }
        }
    }
}

std::wstring Report()
{
    std::wstring out = std::format(L"{}; the field actions {}; component ticks: {} on the game thread, {} on workers",
                                   g_installed ? L"installed" : L"not installed",
                                   g_moved ? (g_onGame.load() ? L"moved to the game thread (seen there)" : L"moved, not seen yet")
                                           : L"where the game puts them",
                                   g_gameTicks, g_workerTicks.load());
    if (g_cost.gameMs >= 0)
    {
        out += std::format(L"; time inside them per frame (last 5 s): {:.3f} ms on the game thread, {:.3f} ms on workers (summed)", g_cost.gameMs,
                           g_cost.workerMs);
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const SeatState& s = g_seats[seat];
        out += std::format(L"; {}: {} now, {} action(s), {} start/exit call(s) with its own leader, {} camera(s) eased out, {} switched off, "
                           L"{} lock(s) skipped",
                           players::SeatName(seat), s.action == 0 ? L"no action" : ActionName(s.action), s.actions, s.leaderResumed,
                           s.camerasLeft, s.camerasOff, s.locksSkipped);
    }
    return out;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto fail = [](const wchar_t* what) {
        platform::LogWarning(L"traversal: {} is not as the research read it; further players' members take ladders and squeezes "
                             L"by themselves, with no pad and no camera of their own", what);
        return false;
    };
    const auto fas        = image.Rebase(facts::kFieldActionSystemGlobal, 8);
    const auto root       = image.Rebase(facts::kEndGameRootGlobal, 8);
    const auto party      = image.Rebase(facts::kPartyManagerGlobal, 8);
    const auto compTick   = platform::FindFunction(image, facts::kFieldActionCompTick);
    const auto tickCall   = image.Rebase(facts::kFieldActionCompTickCall, 5);
    const auto startEnd   = platform::FindFunction(image, facts::kActionStartEnd);
    const auto deactivate = image.Rebase(facts::kFieldCameraDeactivateFlavor, facts::kFieldCameraDeactivateFlavorEntry.size());
    const auto lockSet    = image.Rebase(facts::kFieldControlLockSet), lockClear = image.Rebase(facts::kFieldControlLockClear);
    const auto queueSet   = image.Rebase(facts::kQueueLockBitSet), queueClear = image.Rebase(facts::kQueueLockBitClear);
    const auto crawlLock  = image.Rebase(facts::kCrawlLockCall, 5), crawlUnlock = image.Rebase(facts::kCrawlUnlockCall, 5);
    const auto sqLock     = image.Rebase(facts::kSqueezeLockCall, 5), sqUnlock = image.Rebase(facts::kSqueezeUnlockCall, 5);
    if (!fas || !root || !party || !compTick || !tickCall || !startEnd || !deactivate || !lockSet || !lockClear || !queueSet ||
        !queueClear || !crawlLock || !crawlUnlock || !sqLock || !sqUnlock ||
        !image.BytesMatch(facts::kFieldCameraDeactivateFlavor, facts::kFieldCameraDeactivateFlavorEntry))
    {
        return fail(L"a function, call site or global");
    }
    g_fasGlobal  = *fas;
    g_root       = *root;
    g_party      = *party;
    g_deactivate = reinterpret_cast<DeactivateFn>(*deactivate);
    if (const auto leave = platform::FindFunction(image, platform::FunctionFact{L"FEndCameraFlavorDuct_BeginLeave", facts::kDuctFlavorLeave,
                                                                                facts::kDuctFlavorLeaveEntry}))
    {
        g_ductLeave = reinterpret_cast<FlavorLeaveFn>(*leave);
    }
    else
    {
        platform::LogWarning(L"traversal: the duct camera's leave is not as the research read it; a further player's camera jumps "
                             L"back after a squeeze");
    }
    g_lockSet = *lockSet, g_lockClear = *lockClear, g_queueSet = *queueSet, g_queueClear = *queueClear;
    g_crawlLockSite = *crawlLock, g_crawlUnlockSite = *crawlUnlock, g_squeezeLockSite = *sqLock, g_squeezeUnlockSite = *sqUnlock;
    if (!g_startEnd.Install(*startEnd, std::span(facts::kActionStartEndEntry.data(), facts::kActionStartEndFirst),
                            reinterpret_cast<std::uintptr_t>(&StartEnd)))
    {
        return fail(L"the action start's first instruction");
    }
    if (!g_compTick.Install(*tickCall, *compTick, reinterpret_cast<std::uintptr_t>(&CompTick)))
    {
        g_startEnd.Restore();
        return fail(L"the component tick's call");
    }
    if (const auto beamEnd = platform::FindFunction(image, facts::kBalanceEndDelegate);
        !beamEnd || !g_balanceEnd.Install(*beamEnd, std::span(facts::kBalanceEndDelegateEntry.data(), facts::kBalanceEndDelegateFirst),
                                          reinterpret_cast<std::uintptr_t>(&BalanceEnd)))
    {
        platform::LogWarning(L"traversal: the balance beam's end is not as the research read it; a further player's beam camera "
                             L"switches off at the end instead of easing out");
    }
    if (const auto nearExit = platform::FindFunction(image, facts::kSqueezeNearExitDelegate);
        !nearExit || !g_squeezeNearExit.Install(*nearExit, std::span(facts::kSqueezeNearExitDelegate.entryBytes.data(),
                                                                          facts::kSqueezeNearExitDelegateFirst),
                                                reinterpret_cast<std::uintptr_t>(&SqueezeNearExit)))
    {
        platform::LogWarning(L"traversal: the squeeze's near-exit call is not as the research read it; a further player's squeeze "
                             L"camera turns to the member's facing until the squeeze ends");
    }
    const auto crawlJump   = image.Rebase(facts::kCrawlNearExitJump, 5);
    const auto crawlTarget = image.Rebase(facts::kCrawlNearExitDelegate);
    if (!crawlJump || !crawlTarget || !g_crawlNearExit.InstallTailJump(*crawlJump, *crawlTarget, reinterpret_cast<std::uintptr_t>(&CrawlNearExit)))
    {
        platform::LogWarning(L"traversal: the crawl's near-exit call is not as the research read it; a further player's crawl camera "
                             L"switches off at the end instead of easing out near the exit");
    }
    const auto ladderJump  = image.Rebase(facts::kLadderPhaseJump, 5);
    const auto ladderPhase = platform::FindFunction(image, facts::kLadderPhaseDelegate);
    if (const auto leave = platform::FindFunction(image, facts::kLadderLeave))
    {
        g_ladderLeave = reinterpret_cast<LadderLeaveFn>(*leave);
    }
    else
    {
        platform::LogWarning(L"traversal: the ladder camera's leave is not as the research read it; a further player's ladder camera "
                             L"cuts back after a climb that ends before its exit");
    }
    if (!ladderJump || !ladderPhase ||
        !g_ladderPhase.InstallTailJump(*ladderJump, *ladderPhase, reinterpret_cast<std::uintptr_t>(&LadderPhase)))
    {
        platform::LogWarning(L"traversal: the ladder's phase call is not as the research read it; a further player's ladder camera "
                             L"eases out after the climb instead of at the exit");
    }
    const auto postSlot = image.Rebase(facts::kSlideDownPostTickSlot, 8);
    const auto postTick = platform::FindFunction(image, facts::kSlideDownPostTick);
    if (!postSlot || !postTick ||
        !g_slideDownPostTick.Replace(*postSlot, *postTick, reinterpret_cast<std::uintptr_t>(&SlideDownPostTick)))
    {
        platform::LogWarning(L"traversal: the slide-down's steer is not as the research read it; a further player's slide-down "
                             L"steers by player 1's stick");
    }
    // The monkey bars go in together: a Start without its Exit would leave the camera to a plain switch-off, an Exit
    // without its base-exit call would send the leader's end signals for a seat.
    const auto baseExitCall = image.Rebase(facts::kMonkeyBarsBaseExitCall, 5);
    const auto baseExit     = image.Rebase(facts::kStateBaseExit);
    if (!baseExitCall || !baseExit || !g_monkeyBarsBaseExit.Install(*baseExitCall, *baseExit, reinterpret_cast<std::uintptr_t>(&MonkeyBarsBaseExit)) ||
        !ReplaceStateSlot(image, facts::kMonkeyBarsStartSlot, facts::kMonkeyBarsStart, g_monkeyBarsStart, &MonkeyBarsStart) ||
        !ReplaceStateSlot(image, facts::kMonkeyBarsExitSlot, facts::kMonkeyBarsExit, g_monkeyBarsExit, &MonkeyBarsExit))
    {
        const bool startBack = g_monkeyBarsStart.Restore();
        const bool back      = g_monkeyBarsBaseExit.Restore() && startBack;
        platform::LogWarning(L"traversal: the monkey bars' start or exit is not as the research read it; a further player on monkey "
                             L"bars keeps the follow camera{}", back ? L"" : L" (a slot could not be put back; it passes through)");
    }
    if (!ReplaceStateSlot(image, facts::kBalanceStartSlot, facts::kBalanceStart, g_balanceStart, &BalanceStart))
    {
        platform::LogWarning(L"traversal: the balance beam's start is not as the research read it; a further player on a beam keeps "
                             L"the follow camera");
    }
    g_installed = true;
    control::Register({.name  = L"traversal.action",
                       .shape = control::Shape::Text,
                       .help  = L"where the field actions tick, and each further player's member's current action and counts",
                       .get   = [] { return control::Value{Report()}; }});
    platform::Log(L"traversal: installed; further players' members run their walk-up actions on their own pads and cameras");
    return true;
}

void TickSeat(int seat, void* member) noexcept
{
    if (!g_installed || !players::SeatBuilt(seat))
    {
        return;
    }
    SeatState& state = g_seats[seat];
    state.member     = member;
    state.component  = member != nullptr ? ReadPointer(member, facts::kCharacterFieldAction) : nullptr;
    g_seatComponents[seat].store(state.component, std::memory_order_relaxed);
    std::uint8_t action = 0;
    if (state.component != nullptr)
    {
        Read(static_cast<const std::uint8_t*>(state.component) + facts::kFieldActionState, action);
    }
    if (const std::uint8_t nearEnd = g_ductNearEnd[seat].exchange(0, std::memory_order_relaxed); nearEnd != 0 && member != nullptr)
    {
        DuctLeaveNearEnd(seat, nearEnd);
    }
    if (void* ladder = g_ladderExit[seat].exchange(nullptr, std::memory_order_acquire); ladder != nullptr && member != nullptr)
    {
        LadderLeaveAtExit(seat, ladder, g_ladderExitPhase[seat].load(std::memory_order_relaxed));
    }
    if (const int thread = g_ladderThread.load(std::memory_order_relaxed); thread != 0 && !g_ladderThreadTold)
    {
        g_ladderThreadTold = true;
        platform::LogDebug(L"traversal: the ladder's phase call runs on {} (research/RE-traversal-phases-two.md live check 3)",
                           thread == 1 ? L"the game thread" : L"another thread");
    }
    if (action == state.action)
    {
        return;
    }
    if (state.action != 0)
    {
        platform::LogDebug(L"traversal: {}'s member {} a {}", players::SeatName(seat), member != nullptr ? L"ends" : L"left during",
                           ActionName(state.action));
        if (member != nullptr)
        {
            CamerasOffAfter(seat, state.action);
        }
    }
    if (action != 0)
    {
        state.ladderKeepYaw = -1;
        ++state.actions;
        platform::LogDebug(L"traversal: {}'s member starts a {}", players::SeatName(seat), ActionName(action));
    }
    state.action = action;
}

void Tick(bool anyIn) noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    CountCost();
    Place(anyIn);
    PlaceLocks();
    WatchSqueezeCameras();
    if (g_squeezeQueued.load(std::memory_order_relaxed) && !PlayerOneIn(facts::kFieldActionSqueeze))
    {
        g_squeezeQueued.store(false, std::memory_order_relaxed);  // player 1's squeeze ended without its unlock (a load, a change)
    }
}

void Remove() noexcept
{
    // The four lock calls stay redirected (see the header): with the component tick restored no seat's component ticks
    // scoped again, so they pass straight through.
    g_balanceStart.Restore();
    g_monkeyBarsExit.Restore();
    g_monkeyBarsStart.Restore();
    g_monkeyBarsBaseExit.Restore();
    g_slideDownPostTick.Restore();
    g_ladderPhase.Restore();
    g_crawlNearExit.Restore();
    g_squeezeNearExit.Restore();
    for (int seat = 0; seat < kSeats; ++seat)
    {
        g_ductNearEnd[seat].store(0, std::memory_order_relaxed);
        g_ladderExit[seat].store(nullptr, std::memory_order_relaxed);
        g_ladderExitPhase[seat].store(0, std::memory_order_relaxed);
    }
    g_balanceEnd.Restore();
    g_compTick.Restore();
    g_startEnd.Restore();
    if (g_moved)
    {
        auto*        fas   = static_cast<std::uint8_t*>(ReadPointer(reinterpret_cast<const void*>(g_fasGlobal), 0));
        std::uint8_t flags = 0;
        if (fas != nullptr && reinterpret_cast<std::uintptr_t>(fas) == g_fas && Read(fas + facts::kFieldActionTickFlags, flags))
        {
            const std::uint8_t set = flags | facts::kTickRunOnAnyThread;
            platform::WriteGuarded(fas + facts::kFieldActionTickFlags, &set, 1);
        }
        g_moved = false;
    }
    g_installed = false;
}

bool OnGameThread() noexcept
{
    return g_onGame.load(std::memory_order_relaxed);
}

bool MoverLockNow() noexcept
{
    return g_crawlLocking || g_squeezeQueued.load(std::memory_order_relaxed);
}

bool Scoping() noexcept
{
    return g_onGame.load(std::memory_order_relaxed) && g_locksOk.load(std::memory_order_relaxed);
}

bool PlayerOneActionHoldsNoAction() noexcept
{
    const std::uint8_t* comp   = nullptr;
    const std::uint8_t* leader = PlayerOneLeader(&comp);
    std::uint8_t        took = 0, keeps = 1;
    return comp != nullptr && Read(comp + facts::kFieldActionTookLocks, took) && took != 0 &&
           Read(leader + facts::kCharacterKeepsFieldActions, keeps) && keeps == 0;
}

std::wstring Reading() noexcept
try
{
    return Report();
}
catch (...)
{
    return L"unreadable";
}

}  // namespace lss::world::traversal_action
