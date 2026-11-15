#include "world/traversal_action.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/field_root.h"
#include "players/game_globals.h"
#include "players/pad_buttons.h"
#include "players/seat_camera.h"
#include "players/scopes.h"
#include "players/seat_objects.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "world/field_targets.h"
#include "world/push_event_rules.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
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
using FinishFn     = void (*)(void** capture);
using EndSignalsFn = std::uint64_t (*)(void* actor, void* list, char reached);
using StartSignalsFn = std::uint64_t (*)(void* spot, const std::uint64_t* tag, char side);
using LadderLeaveFn = void (*)(void* flavor, char keepYaw);

constexpr int           kSeats   = players::scopes::kMaxPlayers;
constexpr std::uint8_t  kFlavors[] = {facts::kFlavorLadder, facts::kFlavorDuct, facts::kFlavorRope, facts::kFlavorCustomizable};

struct SeatState
{
    void*         member    = nullptr;
    void*         component = nullptr;
    std::uint8_t  action    = 0;  // the component's running action at the last tick
    std::uint32_t actions = 0, camerasLeft = 0, camerasOff = 0, locksSkipped = 0;  // left: the game's eased leave
    std::uint32_t leaderResumed = 0;  // Start/Exit calls run with the seat's leader inside the action start
    std::int8_t   ladderKeepYaw = -1;  // an exit phase noted as the climb ended: its leave's keep-yaw flag (-1 none)
    bool          withAi        = false;  // the member is handed to its AI: its field actions run as the game's member's
    bool          actionWithAi  = false;  // the running action started while withAi (the AI's, not the player's)
};

// Set at install, then only read.
std::uintptr_t g_fasGlobal = 0;
std::uintptr_t g_party     = 0;
DeactivateFn   g_deactivate = nullptr;
FlavorLeaveFn  g_ductLeave  = nullptr;
LadderLeaveFn  g_ladderLeave = nullptr;
FlavorLeaveFn  g_ropeLeave   = nullptr;
std::uintptr_t g_lockSet = 0, g_lockClear = 0, g_queueSet = 0, g_queueClear = 0;
std::uintptr_t g_crawlLockSite = 0, g_crawlUnlockSite = 0, g_squeezeLockSite = 0, g_squeezeUnlockSite = 0;
bool           g_installed = false;

platform::CallRedirect  g_compTick;
platform::EntryRedirect g_startEnd;
platform::EntryRedirect g_startSignals;  // a spot's start signals: a seat's member's sent as the game's member's
std::uint32_t           g_startSignalsAsMember = 0, g_startSignalsBoard = 0;
platform::CallRedirect  g_crawlLock, g_crawlUnlock, g_squeezeLock, g_squeezeUnlock;
platform::CallRedirect  g_hookShotLock, g_hookShotUnlock, g_hookShotAbortUnlock;
bool                    g_hookShotReady = false;  // the three placed: a seat's hookshot takes no field lock
platform::VTableEntry   g_slideDownPostTick;
platform::EntryRedirect g_balanceEnd;
platform::EntryRedirect g_squeezeNearExit;
platform::CallRedirect  g_crawlNearExit;
platform::VTableEntry   g_monkeyBarsStart, g_monkeyBarsExit, g_balanceStart;
platform::CallRedirect  g_monkeyBarsBaseExit;
platform::CallRedirect  g_ladderPhase;
platform::EntryRedirect g_gapJumpFinish, g_poleFinish;
platform::VTableEntry   g_fenceStart, g_fenceExit;
platform::CallRedirect  g_fenceEndSignals, g_fenceBaseStart;
platform::VTableEntry   g_boardExit;  // the operation board's Exit: its end trigger as the seat's leader's (the level listens for it)
bool                    g_poleReady = false;  // the pole's finish fix and its camera leave installed

platform::GameThreadMark g_gameThread;
std::atomic<int>   g_ticking{0};       // the seat whose member's component the game thread is ticking now
std::atomic<bool>  g_onGame{false};    // the field-action tick ran on the game thread since the mod moved it
std::atomic<std::uint32_t> g_workerTicks{0};
std::array<std::atomic<void*>, kSeats> g_seatComponents{};  // each seat's member's component, for the animation side
std::array<std::atomic<bool>, kSeats>  g_seatWithAi{};      // the seat's member runs as the game's member (SeatState::withAi)
// Written on the game thread, read on any (a state's slot can be called on a worker while the tick is not moved).
std::atomic<int>   g_pausedSeat{0};    // the seat whose action start runs now with player 1's leader
std::atomic<int>   g_resumedSeat{0};   // the seat whose Start or Exit runs now with its own leader again
std::array<std::atomic<std::uint8_t>, kSeats> g_ductNearEnd{};  // the action (squeeze, crawl) whose near-exit call ran for a seat's member
std::array<std::atomic<void*>, kSeats>        g_ladderExit{};       // a seat's member's ladder state that reached an exit phase
std::array<std::atomic<std::uint8_t>, kSeats> g_ladderExitPhase{};  // that phase (written before the state)
std::array<std::atomic<bool>, kSeats>         g_poleFinished{};     // a seat's member's pole slide finished (its camera's leave)
std::array<std::atomic<bool>, kSeats>         g_gapJumpFinished{};  // a seat's member's long jump finished (set done only)
std::atomic<int> g_finishThread{0};  // the jump's or pole's finish seen on: 1 the game thread, 2 another (logged once)
bool             g_finishThreadTold = false;
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
std::atomic<bool> g_hookShotQueued{false};  // player 1's hookshot turn lock queued and not yet unqueued (any thread)
std::atomic<std::uint8_t> g_squeezeBit{0}, g_hookShotBit{1};  // the queued lock bit player 1's squeeze and hookshot took last
std::atomic<std::uint32_t> g_crawlBits{0};  // the mask player 1's crawl lock holds (0: none; its unlock or its end clears it)
std::uint32_t  g_moveSeen = 0;  // lock bits seen standing for player 1's own move, kept while each stays set (game thread)
std::array<bool, kSeats> g_seatNoAction{};     // game thread: the seat's own action held "no new action" at its last run
std::array<bool, kSeats> g_seatActorUnread{};  // game thread: a seat's running action's actor could not be read (logged once)
bool           g_neverMovable = false;  // a FieldActionSystem whose tick never ran on any thread: logged once
std::uint32_t  g_gameTicks = 0;
std::int64_t   g_gameTime  = 0;  // performance-counter ticks spent inside component ticks on the game thread

/// The frame cost of the component ticks, per 5 s window (game thread): what moving the tick to the game thread costs.
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
    return g_gameThread.Here();
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
            const players::seat_camera::StickScope stick(static_cast<std::uint32_t>(seat), true);
            const players::pad_buttons::ButtonScope      buttons(static_cast<unsigned>(seat));
            auto*                                        root = players::field_root::Root();
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

/// The seat whose member owns `component` and plays it now (game thread), or 0: a member handed to its AI is the game's.
int SeatOfComponent(const void* component) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (component != nullptr && component == g_seats[seat].component && !g_seats[seat].withAi)
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

/// A seat's member's component (any thread), or 0; as SeatOfComponent, a member handed to its AI is the game's.
int SeatOfComponentAnyThread(const void* component) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (component != nullptr && component == g_seatComponents[seat].load(std::memory_order_relaxed) &&
            !g_seatWithAi[seat].load(std::memory_order_relaxed))
        {
            return seat;
        }
    }
    return 0;
}

/// The seat whose member's component a field action state belongs to, while the seats' scopes run (any thread), or 0.
int SeatOfState(const void* state) noexcept
{
    return state != nullptr && Scoping() ? SeatOfComponentAnyThread(players::actor_fields::ActionStateComponent(state)) : 0;
}

/// `state` belongs to `seat`'s own member, on the game thread.
bool SeatsOwnState(int seat, const void* state) noexcept
{
    return seat != 0 && OnGameThreadNow() && players::actor_fields::ActionStateComponent(state) == g_seats[seat].component;
}

/// The balance beam's end delegate (animation side): for a seat's member, its stored camera is kept out of the
/// delegate's test of player 1's camera, so the state's exit (inside the action start, on the seat's operator; it has no
/// leader test) eases the seat's camera out.
void BalanceEnd(void** capture, char reached)
{
    const auto original = reinterpret_cast<BalanceEndFn>(g_balanceEnd.Original());
    auto*      state    = capture != nullptr ? static_cast<std::uint8_t*>(ReadPointer(capture, 0)) : nullptr;
    const int  seat     = SeatOfState(state);
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
    if (const int seat = SeatOfState(state); seat != 0)
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
    if (const int seat = SeatOfState(state); seat != 0)
    {
        g_ladderThread.store(OnGameThreadNow() ? 1 : 2, std::memory_order_relaxed);  // research: which thread the animation side is
        g_ladderExitPhase[seat].store(reached, std::memory_order_relaxed);
        g_ladderExit[seat].store(state, std::memory_order_release);
    }
}

/// A seat's member's state (animation side): the seat, or 0.
int SeatOfStateAnyThread(void* const* capture, void** state) noexcept
{
    *state = capture != nullptr ? ReadPointer(capture, 0) : nullptr;
    return SeatOfState(*state);
}

/// The gap jump's long-jump finish (animation side): the game sets the state done and ticks the owner's component there,
/// outside the seat's scopes (the jump's end, the queued slide). For a seat's member only the done byte is set: the
/// member's next tick on the game thread, in its scopes, ends the jump and starts the slide (up to a frame later).
void GapJumpFinish(void** capture)
{
    void*                  state = nullptr;
    constexpr std::uint8_t done  = 1;
    if (const int seat = SeatOfStateAnyThread(capture, &state);
        seat != 0 && platform::WriteGuarded(static_cast<std::uint8_t*>(state) + facts::kStateDone, &done, 1))
    {
        g_finishThread.store(OnGameThreadNow() ? 1 : 2, std::memory_order_relaxed);
        g_gapJumpFinished[seat].store(true, std::memory_order_relaxed);
        return;
    }
    reinterpret_cast<FinishFn>(g_gapJumpFinish.Original())(capture);  // a write that faulted: the game's own end
}

/// The pole slide's finish (animation side): its "was the leader" byte was captured in the seat's scopes, so the game
/// would leave player 1's pole camera from here. For a seat's member the byte is cleared first (done only), and the
/// seat's own camera leave runs on the game thread next tick.
void PoleFinish(void** capture)
{
    void*     state = nullptr;
    const int seat  = SeatOfStateAnyThread(capture, &state);
    if (seat != 0)
    {
        constexpr std::uint8_t no = 0;
        platform::WriteGuarded(reinterpret_cast<std::uint8_t*>(capture) + facts::kPoleFinishWasLeader, &no, 1);
    }
    reinterpret_cast<FinishFn>(g_poleFinish.Original())(capture);
    if (seat != 0)
    {
        g_finishThread.store(OnGameThreadNow() ? 1 : 2, std::memory_order_relaxed);
        g_poleFinished[seat].store(true, std::memory_order_relaxed);
    }
}

/// The slide-down's post-tick (its side steer, outside the component tick): a seat's member's runs as its component
/// tick does, so pad 2 or 3 steers it.
void SlideDownPostTick(void* state, float dt)
{
    const auto original = reinterpret_cast<PostTickFn>(g_slideDownPostTick.Original());
    const int  seat     = OnGameThreadNow() && g_locksOk.load(std::memory_order_relaxed) ? SeatOfComponent(players::actor_fields::ActionStateComponent(state)) : 0;
    if (seat == 0)
    {
        original(state, dt);
        return;
    }
    RunAsSeat(seat, [&] { original(state, dt); });
}

/// The seat whose member's component the game thread is ticking now, asked on the game thread; 0 otherwise.
int TickingSeat() noexcept
{
    const int seat = g_ticking.load(std::memory_order_relaxed);
    return seat != 0 && OnGameThreadNow() ? seat : 0;
}

/// The ticking seat whose member owns `component`, on the game thread; 0 otherwise.
int TickingSeatOf(const void* component) noexcept
{
    const int seat = TickingSeat();
    return seat != 0 && component == g_seats[seat].component ? seat : 0;
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

/// A spot's start signals, sent from the action's state enters inside the component tick for the character whose first
/// tag `tag` points at (a copy of that name, not the character). For the party leader's name the game sends the spot's
/// start signals, which set level states the save keeps (a spot's camera on player 1's camera manager among them); the
/// matching end signals go from the state's Exit, which a seat's action runs with player 1's leader (StartEnd), so a
/// seat's member sending the leader's start would leave that state on for good (research/RE-camera-slot14-stuck.md,
/// RE-cave-spot-camera-toggle.md). A seat's member's start therefore runs with player 1's leader too: the game's
/// non-leader branch, an AI member's, and start and end match. A switch board keeps the leader's: its Exit sends the
/// leader's end for the seat (BoardExit), the trigger the level listens for. Any other call is the game's.
std::uint64_t StartSignals(void* spot, const std::uint64_t* tag, char side)
{
    const auto          original = reinterpret_cast<StartSignalsFn>(g_startSignals.Original());
    const int           seat     = TickingSeat();
    const std::uint64_t own      = seat != 0 && g_seats[seat].member != nullptr ? world::field_targets::FirstTag(g_seats[seat].member) : 0;
    std::uint64_t       who      = 0;
    if (own == 0 || tag == nullptr || !Read(tag, who) || who != own)
    {
        return original(spot, tag, side);
    }
    std::uint64_t name = 0;
    Read(static_cast<const std::uint8_t*>(spot) + facts::kObjectName, name);
    const std::wstring spotName = world::field_targets::NameOf(name);
    if (push_event_rules::OperationBoard(spotName))
    {
        ++g_startSignalsBoard;
        return original(spot, tag, side);
    }
    const players::scopes::LeaderPause pause(seat);  // a no-op inside the action start's own pause: the member branch already
    if (pause.Paused())
    {
        ++g_startSignalsAsMember;
        platform::LogDebug(L"traversal: {}'s member starts at {}: its start signals go as the game's member's (the spot's level "
                           L"states stay player 1's)",
                           players::SeatName(seat), spotName);
    }
    return original(spot, tag, side);
}

/// A state's own Start or Exit (`slot` holds the game's). Called by the action start for a seat's member, while the mod
/// holds player 1's leader there, it runs with the seat's leader again for this one call: its camera code (the leader
/// type's) then answers for the seat, on the seat's operator. Any other call is the game's. True if it ran resumed.
bool WithSeatLeader(const platform::VTableEntry& slot, void* state, const wchar_t* what)
{
    const auto original = reinterpret_cast<StateCallFn>(slot.Original());
    const int  seat     = g_pausedSeat.load(std::memory_order_relaxed);
    if (!SeatsOwnState(seat, state))
    {
        original(state);
        return false;
    }
    const players::scopes::Scope leader(seat, players::scopes::Leader);
    if (!leader.Held())
    {
        original(state);  // player 1's leader, as before: no camera for the seat
        return false;
    }
    ++g_seats[seat].leaderResumed;
    platform::LogDebug(L"traversal: {}'s {} runs with its own member as the leader (its camera)", players::SeatName(seat), what);
    const SeatMark resumed(g_resumedSeat, seat);
    original(state);
    return true;
}

/// The monkey bars' Exit's first call, the base exit: its end signals choose the leader's set or the member's by the
/// leader type, so inside a resumed Exit it runs with player 1's leader again, as every seat's action end does
/// (a level script listening for the leader's end stays player 1's).
void MonkeyBarsBaseExit(void* state)
{
    const auto original = reinterpret_cast<StateCallFn>(g_monkeyBarsBaseExit.Original());
    const int  seat     = g_resumedSeat.load(std::memory_order_relaxed);
    if (!SeatsOwnState(seat, state))
    {
        original(state);
        return;
    }
    const players::scopes::LeaderPause pause(seat);
    original(state);
}

const std::uint8_t* PlayerOneLeader(const std::uint8_t** component) noexcept;

/// The actor a state acts on (its weak pair at +0x120; the Start sets it, the Exit clears it at its end), or null.
std::uint8_t* StateActor(const void* state) noexcept
{
    std::int32_t pair[2]{-1, 0};
    if (!platform::CopyGuarded(pair, static_cast<const std::uint8_t*>(state) + facts::kStateActorWeak, sizeof(pair)))
    {
        return nullptr;
    }
    return static_cast<std::uint8_t*>(players::weak_object::ObjectOf(pair));
}

/// Player 1's leader climbs this wire fence now (its own fence state names the actor): the value the game keeps in the
/// actor's "the party leader is on this mesh" byte, which only the party leader's Start and Exit write.
bool PlayerOneOnFence(const void* actor) noexcept
{
    const std::uint8_t* comp   = nullptr;
    std::uint8_t        action = 0;
    if (PlayerOneLeader(&comp) == nullptr || comp == nullptr || !Read(comp + facts::kFieldActionState, action) ||
        action != facts::kFieldActionWireFence)
    {
        return false;
    }
    const void* state = ReadPointer(comp, facts::kFieldActionStates + 8u * facts::kFieldActionWireFence);
    return state != nullptr && StateActor(state) == actor;
}

/// The wire fence's Start or Exit with the seat's leader again (its camera). The actor's "the party leader is on this
/// mesh" byte, which AI members follow, is then set back to what player 1's own climb makes it: never a seat's member.
void WireFenceCall(const platform::VTableEntry& slot, void* state, const wchar_t* what)
{
    const int  seat  = g_pausedSeat.load(std::memory_order_relaxed);
    const bool mine  = SeatsOwnState(seat, state);
    std::uint8_t* actor = mine ? StateActor(state) : nullptr;  // the Exit's: cleared by the call
    if (!WithSeatLeader(slot, state, what) || !mine)
    {
        return;  // the game ran it with player 1's leader: it left the byte as the game keeps it
    }
    if (actor == nullptr)
    {
        actor = StateActor(state);  // the Start's: set by the call
    }
    std::uint8_t on = 0;
    if (actor == nullptr || !Read(actor + facts::kWireFenceLeaderOn, on))
    {
        platform::LogDebug(L"traversal: {}'s {}: the fence could not be found; its AI follow flag left as the call left it",
                           players::SeatName(seat), what);
        return;
    }
    const std::uint8_t want = PlayerOneOnFence(actor) ? 1 : 0;
    if (on != want && platform::WriteGuarded(actor + facts::kWireFenceLeaderOn, &want, 1))
    {
        platform::LogDebug(L"traversal: {}'s {}: the fence's AI follow flag put back to player 1's ({})", players::SeatName(seat), what,
                           want);
    }
}

void WireFenceStart(void* state) { WireFenceCall(g_fenceStart, state, L"wire-fence start"); }
void WireFenceExit(void* state) { WireFenceCall(g_fenceExit, state, L"wire-fence exit"); }

/// The wire fence's Start's base start (its tail call takes the leader-typed OnOff 25, a global switch): inside a resumed
/// Start it runs with player 1's leader again.
void WireFenceBaseStart(void* state)
{
    const auto original = reinterpret_cast<StateCallFn>(g_fenceBaseStart.Original());
    const int  seat     = g_resumedSeat.load(std::memory_order_relaxed);
    if (!SeatsOwnState(seat, state))
    {
        original(state);
        return;
    }
    const players::scopes::LeaderPause pause(seat);
    original(state);
}

/// The wire fence's Exit's end signals (a direct call: the leader's set or the member's by the leader type): inside a
/// resumed Exit they run with player 1's leader again, as every seat's action end does. Its one call site is inside the
/// fence's Exit, so the resumed seat is enough (its arguments name no state).
std::uint64_t WireFenceEndSignals(void* actor, void* list, char reached)
{
    const auto original = reinterpret_cast<EndSignalsFn>(g_fenceEndSignals.Original());
    const int  seat     = g_resumedSeat.load(std::memory_order_relaxed);
    if (seat == 0 || !OnGameThreadNow())
    {
        return original(actor, list, reached);
    }
    const players::scopes::LeaderPause pause(seat);
    return original(actor, list, reached);
}

void MonkeyBarsStart(void* state) { WithSeatLeader(g_monkeyBarsStart, state, L"monkey-bars start"); }
void MonkeyBarsExit(void* state) { WithSeatLeader(g_monkeyBarsExit, state, L"monkey-bars exit"); }
void BalanceStart(void* state) { WithSeatLeader(g_balanceStart, state, L"balance-beam start"); }
/// The operation board's Exit (a switch board's quick press): unlike the other actions' ends, its end trigger is what the
/// level listens for (a door opening), and the game sends it only as the party leader's, so a seat's runs with the seat's
/// leader (world/push_events.h).
void BoardExit(void* state) { WithSeatLeader(g_boardExit, state, L"switch board's exit (its end trigger sent as the seat's)"); }

/// Points a state's vtable slot at `thunk` if it holds the function the research read. False (nothing changed) otherwise.
bool ReplaceStateSlot(const platform::GameImage& image, std::uintptr_t slotFact, const platform::FunctionFact& target,
                      platform::VTableEntry& entry, StateCallFn thunk) noexcept
{
    const auto slot = image.Rebase(slotFact, 8);
    const auto fn   = platform::FindFunction(image, target);
    return slot && fn && entry.Replace(*slot, *fn, reinterpret_cast<std::uintptr_t>(thunk));
}

/// A crawl's, squeeze's or grappling hook's lock or unlock call: skipped while a seat's member's component ticks (game
/// thread).
bool SkipLock() noexcept
{
    const int seat = TickingSeat();
    if (seat == 0)
    {
        return false;
    }
    ++g_seats[seat].locksSkipped;
    platform::LogDebug(L"traversal: {}'s crawl, squeeze or hookshot lock call skipped (a lock would hand the party leader to its AI, an "
                       L"unlock would end player 1's)", players::SeatName(seat));
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
    if (mine)
    {
        g_crawlBits.store(static_cast<std::uint32_t>(mask), std::memory_order_relaxed);  // held for the whole crawl
    }
    const auto result = reinterpret_cast<LockFn>(g_crawlLock.Original())(stateManager, mask, reason);
    g_crawlLocking  = false;
    return result;
}

std::uint64_t CrawlUnlock(void* stateManager, std::int32_t mask, std::uint64_t reason)
{
    if (SkipLock())
    {
        return 0;
    }
    g_crawlBits.store(0, std::memory_order_relaxed);
    return reinterpret_cast<LockFn>(g_crawlUnlock.Original())(stateManager, mask, reason);
}

void SqueezeLock(std::int8_t bit, std::uint64_t reason)
{
    if (!SkipLock())
    {
        if (OnGameThreadNow())
        {
            g_squeezeBit.store(static_cast<std::uint8_t>(bit), std::memory_order_relaxed);
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

/// The hookshot's turn lock: skipped for a seat's member; player 1's is a lock of the mover only (MoverLockNow). A seat's
/// Start runs inside the action start under the pause, so its leader-typed Set is normally never reached: the skip that
/// matters for a seat is its sub-state 1 Clear (scoped), which would clear player 1's turn lock early.
void HookShotLock(std::int8_t bit, std::uint64_t reason)
{
    if (!SkipLock())
    {
        if (OnGameThreadNow())
        {
            g_hookShotBit.store(static_cast<std::uint8_t>(bit), std::memory_order_relaxed);
            g_hookShotQueued.store(true, std::memory_order_relaxed);
        }
        reinterpret_cast<QueueLockFn>(g_hookShotLock.Original())(bit, reason);
    }
}

/// Its two unlocks (after the turn, and the Exit's at every end). Both redirects were placed checking the same target,
/// the queued lock's clear, so this one thunk calls it directly.
void HookShotUnlock(std::int8_t bit, std::uint64_t reason)
{
    if (!SkipLock())
    {
        g_hookShotQueued.store(false, std::memory_order_relaxed);
        reinterpret_cast<QueueLockFn>(g_queueClear)(bit, reason);
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
        case facts::kFieldActionGapJump: return L"gap jump";
        case facts::kFieldActionPole: return L"pole slide";
        case facts::kFieldActionWireFence: return L"wire-fence climb";
        case facts::kFieldActionHookShot: return L"grappling hook";
        case facts::kFieldActionCurtain: return L"curtain";
        default: break;
    }
    // A kind the table does not name (the guide's gate, for one) gives its number. The text lives in this thread's
    // buffer until its next unnamed kind; every caller uses it in one log line at once.
    thread_local wchar_t text[32]{};
    swprintf_s(text, L"field action (kind %u)", static_cast<unsigned>(action));
    return text;
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
    const void*  op     = players::seat_camera::Operator(seat);
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

/// The rope/pole camera is leaving already: its current phase is past kind 1, or a way out is pending.
bool RopeLeaving(const void* flavor) noexcept
{
    const void*  current = ReadPointer(flavor, facts::kRopePhaseCurrent);
    const void*  pending = ReadPointer(flavor, facts::kRopePhasePending);
    std::int32_t kind    = 0;
    return (current != nullptr && Read(current, kind) && kind > 1) || (pending != nullptr && Read(pending, kind) && kind > 1);
}

/// The seat's rope/pole camera eased out with the game's own leave, unless it is leaving already.
void RopeLeave(int seat, const wchar_t* when) noexcept
{
    const void* op     = players::seat_camera::Operator(seat);
    void*       flavor = op != nullptr ? ReadPointer(op, facts::kOperatorFlavors + 8u * facts::kFlavorRope) : nullptr;
    std::uint8_t active = 0;
    if (flavor == nullptr || g_ropeLeave == nullptr || !Read(static_cast<const std::uint8_t*>(flavor) + facts::kFlavorActive, active) ||
        active == 0 || RopeLeaving(flavor))
    {
        return;
    }
    if (platform::CallGuarded(g_ropeLeave, flavor))
    {
        ++g_seats[seat].camerasLeft;
        platform::LogDebug(L"traversal: {}'s pole/rope camera eases out {}", players::SeatName(seat), when);
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
    const void*  op        = players::seat_camera::Operator(seat);
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

/// A camera flavor's active byte on an operator (-1 unread).
int FlavorActive(const void* op, std::uint8_t id) noexcept
{
    const void*  flavor = op != nullptr ? ReadPointer(op, facts::kOperatorFlavors + 8u * id) : nullptr;
    std::uint8_t active = 0;
    return flavor != nullptr && Read(static_cast<const std::uint8_t*>(flavor) + facts::kFlavorActive, active) ? active : -1;
}

/// A seat's grappling hook starting or ending: the hookshot camera on its player's operator and on player 1's
/// (the seat's goes on and off, player 1's stays off); whether the seat's
/// controller possesses the member (the game's own move hold during the turn needs it)
/// and the member's mesh tilt (a flight without its landing leaves it tilted).
void LogHookShotCameras(int seat, const void* member, const wchar_t* when) noexcept
{
    const void* controller = players::seat_objects::Controller(seat);
    const void* possessed  = ReadPointer(controller, facts::kControllerCharacter);  // null-safe
    const auto* mesh      = players::actor_fields::Mesh(member);
    float       tilt[3]{};
    const bool  tiltRead = mesh != nullptr && platform::CopyGuarded(tilt, mesh + facts::kSceneRelativeRotation, sizeof(tilt));
    platform::LogDebug(L"traversal: {}'s grappling hook {}: its hookshot camera {}, player 1's {}; its controller possesses the member: {}; "
                       L"mesh pitch {:.1f} roll {:.1f}{}",
                       players::SeatName(seat), when, FlavorActive(players::seat_camera::Operator(seat), facts::kFlavorHookShot),
                       FlavorActive(players::seat_camera::OperatorOne(), facts::kFlavorHookShot),
                       member == nullptr ? L"no member" : controller == nullptr ? L"no controller" : possessed == member ? L"yes" : L"NO",
                       tilt[0], tilt[2], tiltRead ? L"" : L" (unread)");
}

/// After a seat's action ends: the camera flavors its exit left on the seat's operator, switched off by the game.
void CamerasOffAfter(int seat, std::uint8_t ended) noexcept
{
    void* op = players::seat_camera::Operator(seat);
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
            if (ended != facts::kFieldActionMonkeyBars && ended != facts::kFieldActionBalance && ended != facts::kFieldActionWireFence)
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
        if (id == facts::kFlavorRope)
        {
            if (ended == facts::kFieldActionPole || ended == facts::kFieldActionRope)
            {
                RopeLeave(seat, L"after the action");  // the game's leave, eased (none if already leaving)
            }
            continue;  // not this action's (a script's camera on the seat's operator)
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
        g_hookShotQueued.store(false, std::memory_order_relaxed);
        g_crawlBits.store(0, std::memory_order_relaxed);
        g_moveSeen = 0;
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
    *component         = leader != nullptr ? players::actor_fields::FieldAction(leader) : nullptr;
    return leader;
}

/// Whether a component's running action's actor forces its target icons (the leader's action start then leaves "no new
/// action" off): 1 forces, 0 does not or there is no actor (the start takes nothing without one), -1 an actor named that
/// cannot be read now.
int ActorForcesIcons(const std::uint8_t* component) noexcept
{
    std::int32_t pair[2]{-1, 0};
    if (component == nullptr || !platform::CopyGuarded(pair, component + facts::kFieldActionActorWeak, sizeof(pair)))
    {
        return -1;
    }
    if (pair[0] < 0 || pair[1] == 0)
    {
        return 0;  // no actor: the game's start tests a live actor before it takes the switch
    }
    const auto*  actor  = static_cast<const std::uint8_t*>(players::weak_object::ObjectOf(pair));
    std::uint8_t forces = 0;
    if (actor == nullptr || !Read(actor + facts::kFieldActorForceTargetIcon, forces))
    {
        return -1;
    }
    return forces != 0 ? 1 : 0;
}

/// A seat's member runs an action that takes "no new action" as the game's start takes it for its leader. An actor that
/// cannot be read counts as taking it (nearly every actor does; a second request would restart the action), logged once.
bool SeatActionHoldsNoAction(int seat) noexcept
{
    const auto*  comp   = static_cast<const std::uint8_t*>(g_seats[seat].component);
    std::uint8_t action = 0;
    if (comp == nullptr || !Read(comp + facts::kFieldActionState, action) || action == 0)
    {
        return false;
    }
    const int forces = ActorForcesIcons(comp);
    if (forces < 0 && !g_seatActorUnread[seat])
    {
        g_seatActorUnread[seat] = true;
        platform::LogDebug(L"traversal: {}'s {}: its actor could not be read; its targets and starts are held as most actors' are",
                           players::SeatName(seat), ActionName(action));
    }
    return forces != 1;
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
/// read at the squeeze's start and through its first 3 s, so player 1's look and a seat's can be compared (the
/// difference to look for: a seat's camera zooming in and then turning 90 degrees smoothly, where player 1's only
/// zooms). The squeezer's facing is the anchor the camera's hold fades to. Read only; Debug.
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
    if (const void* root = member != nullptr ? players::actor_fields::RootComponent(member) : nullptr;
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
            op        = players::seat_camera::OperatorOne();
        }
        else
        {
            member    = g_seats[player].member;
            component = g_seats[player].component;
            op        = players::seat_camera::Operator(player);
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
    out += std::format(L"; spot start signals of players 2 and 3: {} sent as the game's member's, {} a switch board's as the leader's",
                       g_startSignalsAsMember, g_startSignalsBoard);
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

/// The field control lock mask bits player 1's own crawl, squeeze or grappling hook stands for now (0 when none does).
/// Game thread.
std::uint32_t MoverBitsNow() noexcept
{
    std::uint32_t bits = g_crawlBits.load(std::memory_order_relaxed);
    if (g_squeezeQueued.load(std::memory_order_relaxed))
    {
        bits |= 1u << (g_squeezeBit.load(std::memory_order_relaxed) & 31u);
    }
    if (g_hookShotQueued.load(std::memory_order_relaxed))
    {
        bits |= 1u << (g_hookShotBit.load(std::memory_order_relaxed) & 31u);
    }
    return bits;
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
    const auto party      = players::game_globals::Address(players::game_globals::Global::PartyManager);
    const auto compTick   = platform::FindFunction(image, facts::kFieldActionCompTick);
    const auto tickCall   = image.Rebase(facts::kFieldActionCompTickCall, 5);
    const auto startEnd   = platform::FindFunction(image, facts::kActionStartEnd);
    const auto deactivate = image.Rebase(facts::kFieldCameraDeactivateFlavor, facts::kFieldCameraDeactivateFlavorEntry.size());
    const auto lockSet    = image.Rebase(facts::kFieldControlLockSet), lockClear = image.Rebase(facts::kFieldControlLockClear);
    const auto queueSet   = image.Rebase(facts::kQueueLockBitSet), queueClear = image.Rebase(facts::kQueueLockBitClear);
    const auto crawlLock  = image.Rebase(facts::kCrawlLockCall, 5), crawlUnlock = image.Rebase(facts::kCrawlUnlockCall, 5);
    const auto sqLock     = image.Rebase(facts::kSqueezeLockCall, 5), sqUnlock = image.Rebase(facts::kSqueezeUnlockCall, 5);
    if (!fas || !players::field_root::Installed() || !party || !compTick || !tickCall || !startEnd || !deactivate || !lockSet || !lockClear || !queueSet ||
        !queueClear || !crawlLock || !crawlUnlock || !sqLock || !sqUnlock ||
        !image.BytesMatch(facts::kFieldCameraDeactivateFlavor, facts::kFieldCameraDeactivateFlavorEntry))
    {
        return fail(L"a function, call site or global");
    }
    g_fasGlobal  = *fas;
    g_party      = party;
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
    if (const auto signals = platform::FindFunction(image, facts::kSpotStartSignals);
        !signals || !g_startSignals.Install(*signals, std::span(facts::kSpotStartSignalsEntry.data(), facts::kSpotStartSignalsFirst),
                                            reinterpret_cast<std::uintptr_t>(&StartSignals)))
    {
        platform::LogWarning(L"traversal: a spot's start signals are not as the research read them; a walk-up by player 2 or 3 can "
                             L"leave a spot's level state on for player 1 (a camera on player 1's view)");
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
    if (const auto leave = platform::FindFunction(image, facts::kRopeFlavorLeave))
    {
        g_ropeLeave = reinterpret_cast<FlavorLeaveFn>(*leave);
    }
    // The two finishes copy a run of two instructions (7 bytes): installed here, at the mod's start, before any field action
    // runs, so no thread is ever inside that run while it is replaced (platform/entry_redirect.h's contract).
    if (const auto finish = platform::FindFunction(image, facts::kGapJumpFinish);
        !finish || !g_gapJumpFinish.Install(*finish, std::span(facts::kGapJumpFinish.entryBytes.data(), facts::kGapJumpFinishFirst),
                                            reinterpret_cast<std::uintptr_t>(&GapJumpFinish)))
    {
        platform::LogWarning(L"traversal: the gap jump's finish is not as the research read it; a further player's long jump ends "
                             L"outside that player's scopes");
    }
    if (const auto finish = platform::FindFunction(image, facts::kPoleFinish);
        !g_ropeLeave || !finish ||
        !(g_poleReady = g_poleFinish.Install(*finish, std::span(facts::kPoleFinish.entryBytes.data(), facts::kPoleFinishFirst),
                                             reinterpret_cast<std::uintptr_t>(&PoleFinish))))
    {
        platform::LogWarning(L"traversal: the pole slide's finish or its camera leave is not as the research read it; a further "
                             L"player's pole slide can end player 1's pole camera");
    }
    // The wire fence goes in together: a Start without its Exit would leave the camera and the markers on, an Exit without
    // its end-signal call would send the leader's end signals for a seat.
    const auto fenceSignals = image.Rebase(facts::kWireFenceEndSignalsCall, 5);
    const auto endSignals   = image.Rebase(facts::kEndSignals);
    const auto fenceBase    = image.Rebase(facts::kWireFenceBaseStartCall, 5);
    const auto baseStart    = image.Rebase(facts::kStateBaseStart);
    if (!fenceSignals || !endSignals || !fenceBase || !baseStart ||
        !g_fenceEndSignals.Install(*fenceSignals, *endSignals, reinterpret_cast<std::uintptr_t>(&WireFenceEndSignals)) ||
        !g_fenceBaseStart.Install(*fenceBase, *baseStart, reinterpret_cast<std::uintptr_t>(&WireFenceBaseStart)) ||
        !ReplaceStateSlot(image, facts::kWireFenceStartSlot, facts::kWireFenceStart, g_fenceStart, &WireFenceStart) ||
        !ReplaceStateSlot(image, facts::kWireFenceExitSlot, facts::kWireFenceExit, g_fenceExit, &WireFenceExit))
    {
        const bool startBack = g_fenceStart.Restore();
        const bool baseBack  = g_fenceBaseStart.Restore();
        const bool back      = g_fenceEndSignals.Restore() && startBack && baseBack;
        platform::LogWarning(L"traversal: the wire fence's start or exit is not as the research read it; a further player on a wire "
                             L"fence keeps the follow camera{}", back ? L"" : L" (a slot could not be put back; it passes through)");
    }
    // The hookshot's lock calls (each inside one block, so placed here): together, or the hookshot is not admitted.
    const auto hookLock = image.Rebase(facts::kHookShotLockCall, 5), hookUnlock = image.Rebase(facts::kHookShotUnlockCall, 5);
    const auto hookAbort = image.Rebase(facts::kHookShotExitUnlockCall, 5);
    g_hookShotReady = hookLock && hookUnlock && hookAbort &&
                      g_hookShotLock.Install(*hookLock, *queueSet, reinterpret_cast<std::uintptr_t>(&HookShotLock)) &&
                      g_hookShotUnlock.Install(*hookUnlock, *queueClear, reinterpret_cast<std::uintptr_t>(&HookShotUnlock)) &&
                      g_hookShotAbortUnlock.Install(*hookAbort, *queueClear, reinterpret_cast<std::uintptr_t>(&HookShotUnlock));
    if (!g_hookShotReady)
    {
        const bool abortBack = g_hookShotAbortUnlock.Restore(), unlockBack = g_hookShotUnlock.Restore();
        const bool back      = g_hookShotLock.Restore() && unlockBack && abortBack;
        platform::LogWarning(L"traversal: the grappling hook's lock calls are not as the research read it; further players do not "
                             L"use the grappling hook{}", back ? L"" : L" (a call could not be put back; it passes through)");
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
    if (!ReplaceStateSlot(image, facts::kBoardExitSlot, facts::kBoardExit, g_boardExit, &BoardExit))
    {
        platform::LogWarning(L"traversal: the switch board's exit is not as the research read it; a further player's press of a board "
                             L"plays its animation but the board's door or lift does not answer");
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
    state.component  = member != nullptr ? players::actor_fields::FieldAction(member) : nullptr;
    g_seatComponents[seat].store(state.component, std::memory_order_relaxed);
    std::uint8_t action = 0;
    if (state.component != nullptr)
    {
        Read(static_cast<const std::uint8_t*>(state.component) + facts::kFieldActionState, action);
    }
    // A member handed to its AI (the game's own hand: a lead, a scene; its PLAYER slot empty) is an AI ally as in
    // single player: its component ticks outside the seat's scopes, so the tick's "not the leader" branch starts the
    // nav-link actions its path asks for (a door or gate on a lead). Decided between actions only: a running action
    // finishes the way it started.
    if (action == 0)
    {
        const bool withAi = member != nullptr && players::actor_fields::PlayerSlot(member) == nullptr;
        if (withAi && !state.withAi)
        {
            platform::LogDebug(L"traversal: {}'s member is with its AI: its field actions run as the game's AI ally's (a door or gate "
                               L"on its path opens)",
                               players::SeatName(seat));
        }
        else if (!withAi && state.withAi)
        {
            platform::LogDebug(L"traversal: {}'s member is the player's again: its field actions run as the player's", players::SeatName(seat));
        }
        state.withAi = withAi;
        g_seatWithAi[seat].store(withAi, std::memory_order_relaxed);
    }
    if (const std::uint8_t nearEnd = g_ductNearEnd[seat].exchange(0, std::memory_order_relaxed); nearEnd != 0 && member != nullptr)
    {
        DuctLeaveNearEnd(seat, nearEnd);
    }
    if (void* ladder = g_ladderExit[seat].exchange(nullptr, std::memory_order_acquire); ladder != nullptr && member != nullptr)
    {
        LadderLeaveAtExit(seat, ladder, g_ladderExitPhase[seat].load(std::memory_order_relaxed));
    }
    if (g_poleFinished[seat].exchange(false, std::memory_order_relaxed) && member != nullptr)
    {
        platform::LogDebug(L"traversal: {}'s pole slide finished; player 1's pole camera kept out of it", players::SeatName(seat));
        RopeLeave(seat, L"at the pole's foot");
    }
    if (g_gapJumpFinished[seat].exchange(false, std::memory_order_relaxed))
    {
        platform::LogDebug(L"traversal: {}'s long jump finished: set done only, the member's own tick ends it", players::SeatName(seat));
    }
    if (const int thread = g_finishThread.load(std::memory_order_relaxed); thread != 0 && !g_finishThreadTold)
    {
        g_finishThreadTold = true;
        platform::LogDebug(L"traversal: the jump's and pole's finishes run on {} (research/RE-traversal-deferred-two.md live check 3)",
                           thread == 1 ? L"the game thread" : L"another thread");
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
        platform::LogDebug(L"traversal: {}'s member {} a {}{}", players::SeatName(seat), member != nullptr ? L"ends" : L"left during",
                           ActionName(state.action), state.actionWithAi ? L" (its AI's)" : L"");
        if (state.action == facts::kFieldActionHookShot && !state.actionWithAi)
        {
            LogHookShotCameras(seat, member, L"ended");
        }
        if (member != nullptr && !state.actionWithAi)
        {
            CamerasOffAfter(seat, state.action);  // an AI's action started no camera of the player's
        }
        g_seatActorUnread[seat] = false;  // the next action's unread actor is logged again
    }
    if (action != 0)
    {
        state.ladderKeepYaw = -1;
        state.actionWithAi  = state.withAi;  // the mode CompTick ran its start in (withAi changes between actions only)
        if (!state.actionWithAi)
        {
            ++state.actions;
        }
        platform::LogDebug(L"traversal: {}'s member starts a {}{}", players::SeatName(seat), ActionName(action),
                           state.actionWithAi ? L" (its AI's)" : L"");
        if (action == facts::kFieldActionHookShot && !state.actionWithAi)
        {
            LogHookShotCameras(seat, member, L"started");
        }
    }
    state.action = action;
}

void Tick(bool anyIn) noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_gameThread.Set();
    CountCost();
    Place(anyIn);
    PlaceLocks();
    WatchSqueezeCameras();
    if (g_squeezeQueued.load(std::memory_order_relaxed) && !PlayerOneIn(facts::kFieldActionSqueeze))
    {
        g_squeezeQueued.store(false, std::memory_order_relaxed);  // player 1's squeeze ended without its unlock (a load, a change)
    }
    if (g_hookShotQueued.load(std::memory_order_relaxed) && !PlayerOneIn(facts::kFieldActionHookShot))
    {
        g_hookShotQueued.store(false, std::memory_order_relaxed);  // player 1's hookshot ended without its unlock
    }
    if (g_crawlBits.load(std::memory_order_relaxed) != 0 && !PlayerOneIn(facts::kFieldActionCrawl))
    {
        g_crawlBits.store(0, std::memory_order_relaxed);  // player 1's crawl ended through its unlock task (not redirected)
    }
}

void Remove() noexcept
{
    // The four lock calls stay redirected (see the header): with the component tick restored no seat's component ticks
    // scoped again, so they pass straight through.
    g_boardExit.Restore();
    g_balanceStart.Restore();
    g_monkeyBarsExit.Restore();
    g_monkeyBarsStart.Restore();
    g_monkeyBarsBaseExit.Restore();
    // The hookshot's lock calls stay with the four lock calls (see the header): they pass straight through when no seat's
    // component ticks.
    g_fenceExit.Restore();
    g_fenceStart.Restore();
    g_fenceBaseStart.Restore();
    g_fenceEndSignals.Restore();
    g_poleFinish.Restore();
    g_gapJumpFinish.Restore();
    g_slideDownPostTick.Restore();
    g_ladderPhase.Restore();
    g_crawlNearExit.Restore();
    g_squeezeNearExit.Restore();
    for (int seat = 0; seat < kSeats; ++seat)
    {
        g_ductNearEnd[seat].store(0, std::memory_order_relaxed);
        g_ladderExit[seat].store(nullptr, std::memory_order_relaxed);
        g_ladderExitPhase[seat].store(0, std::memory_order_relaxed);
        g_poleFinished[seat].store(false, std::memory_order_relaxed);
        g_gapJumpFinished[seat].store(false, std::memory_order_relaxed);
    }
    g_balanceEnd.Restore();
    g_compTick.Restore();
    g_startSignals.Restore();
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
    g_crawlBits.store(0, std::memory_order_relaxed);
    g_moveSeen  = 0;
    g_installed = false;
}

bool OnGameThread() noexcept
{
    return g_onGame.load(std::memory_order_relaxed);
}

bool MoverLockNow() noexcept
{
    return g_crawlLocking || g_crawlBits.load(std::memory_order_relaxed) != 0 || g_squeezeQueued.load(std::memory_order_relaxed) ||
           g_hookShotQueued.load(std::memory_order_relaxed);
}

std::uint32_t PlayerOneMoveBitsIn(std::uint32_t mask) noexcept
{
    // A queued unlock drops its flag a frame or more before the processed bit clears: a bit once seen as the move's
    // stays the move's while it stays set.
    g_moveSeen = (g_moveSeen | MoverBitsNow()) & mask;
    return g_moveSeen;
}

bool PlayerOneMoveHolds(std::uint32_t mask) noexcept
{
    const std::uint32_t mine = PlayerOneMoveBitsIn(mask);
    return mask == 0 ? MoverLockNow() : (mask & ~mine) == 0;
}

bool HookShotReady() noexcept
{
    return g_hookShotReady && Scoping();
}

bool Scoping() noexcept
{
    return g_onGame.load(std::memory_order_relaxed) && g_locksOk.load(std::memory_order_relaxed);
}

bool PoleReady() noexcept
{
    return g_poleReady;
}

namespace {

/// Player 1's leader's running action took the "no new action" switch for everyone (one count of it). The start sets
/// "took" for every leader action and the switch only when the action's actor does not force its icons (read from the
/// action's actor, not from the leader character). An actor that cannot be read counts as not taking it.
bool PlayerOneActionHoldsNoAction() noexcept
{
    const std::uint8_t* comp = nullptr;
    std::uint8_t        took = 0;
    return PlayerOneLeader(&comp) != nullptr && comp != nullptr && Read(comp + facts::kFieldActionTookLocks, took) && took != 0 &&
           ActorForcesIcons(comp) == 0;
}

}  // namespace

NoActionScope::NoActionScope(int seat, bool withoutPlayerOne) noexcept
{
    if (seat < 1 || seat > players::kSeatsBuilt || !OnGameThreadNow())
    {
        return;
    }
    auto*       sm      = players::field_root::StateManager();
    auto*       counter = sm != nullptr ? reinterpret_cast<std::int32_t*>(sm + facts::kOnOffTable + 4 * facts::kOnOffDisableFieldAction) : nullptr;
    std::int32_t on     = 0;
    if (counter == nullptr || !Read(counter, on))
    {
        return;
    }
    const bool         playerOne = withoutPlayerOne && on > 0 && PlayerOneActionHoldsNoAction();
    const bool         own       = SeatActionHoldsNoAction(seat);
    const std::int32_t seen      = on - (playerOne ? 1 : 0) + (own ? 1 : 0);
    const bool         written   = seen == on || platform::WriteGuarded(counter, &seen, sizeof(seen));
    if (written && seen != on)
    {
        at_    = counter;
        delta_ = seen - on;
    }
    if (bool& was = g_seatNoAction[seat]; written && was != own)
    {
        was = own;
        platform::LogDebug(L"traversal: {}'s own {} {} its target pick and walk-up starts (the game's \"no new action\", as its leader's action "
                           L"takes it)",
                           players::SeatName(seat), own ? L"action" : L"action's end", own ? L"holds" : L"frees");
    }
}

NoActionScope::~NoActionScope()
{
    // By difference: a script's set or clear made inside the run (the pick calls the actors' notifications) is kept.
    std::int32_t now = 0;
    if (at_ != nullptr && Read(at_, now))
    {
        const std::int32_t back = std::max(now - delta_, 0);
        platform::WriteGuarded(at_, &back, sizeof(back));
    }
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
