#include "world/push_events.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/field_root.h"
#include "players/pad_buttons.h"
#include "players/party_member.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "world/field_targets.h"
#include "world/push_event_rules.h"
#include "world/target_icons.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

namespace lss::world::push_events {

namespace {

using platform::Read;
using platform::ReadPointer;
using players::SeatName;

using InitFn    = char (*)(void* object, const std::uint64_t* targetName, const std::uint64_t* key, const std::uint64_t* actionId,
                        const std::uint64_t* bgActorName, int slot);
using StepFn    = void (*)(void* object, float dt);
using NotifyFn  = std::uint64_t (*)(void* object, void* unused, const std::uint64_t* key);
using FreeFn    = void (*)(void* object);

constexpr int         kSeats       = players::scopes::kMaxPlayers;
constexpr std::size_t kDeferredMax = 8;
constexpr unsigned    kScopes      = players::scopes::Leader | players::scopes::Primary | players::scopes::Operator | players::scopes::UiSlot;
constexpr float         kLoadWaitSeconds      = 5.0f;   // the level-load wait: a level that never loads
constexpr float         kFinishWaitSeconds    = 15.0f;  // the animation waits: a finish that never comes
constexpr std::uint32_t kUnscopedFramesToEnd  = 60;     // a seat's scopes shut this long: the event is ended
constexpr std::size_t   kRowActors            = 3;      // the row getters serve three actor slots

/// The one running event a seat's player started.
struct Event
{
    void*         object      = nullptr;
    int           seat        = 0;
    std::uint64_t actionId    = 0;  // the row's id (the request name the notify carries)
    std::uint64_t target      = 0;  // the target's name, for the log
    std::int32_t  lastStep    = -1;
    float         stepSeconds = 0;  // time in the current step
    std::uint32_t unscopedFrames = 0;  // frames in a row the step could not run in the seat's scopes
    bool          noAnimation = false;
    bool          ended       = false;  // ended by the watchdog or the player's leave
};

/// A finished notify kept for the event's next step (written on the sender's thread, read on the game thread).
struct Deferred
{
    void*         object = nullptr;
    std::uint64_t key    = 0;
};

struct SeatState
{
    void*         localPlayer = nullptr;
    void*         member      = nullptr;
    std::uint32_t events = 0, pressesRefused = 0;
};

// Set at install, then only read.
platform::CallRedirect g_init, g_step, g_notify, g_free;
std::uintptr_t         g_tablePush = 0;  // the push-button row tables' set
CRITICAL_SECTION*      g_lock      = nullptr;
bool                   g_installed = false;

// The member's field AI while its event runs: its follow behaviour reads the party leader every frame, so a member
// that is not the leader turns toward the leader during its hold and the pick drops the lever. Its two per-frame
// functions (the actor tick on the game thread, the update on a worker) are replaced in the four field AI controller
// classes to skip that member; the step runs both inside the seat's scopes instead, where the member is the leader, so
// the behaviour stands as the leader's does in single player.
using AiFn = void (*)(void* controller, float dt);
std::array<platform::VTableEntry, 2 * facts::kFieldAiControllerVtables.size()> g_aiSlots;
AiFn               g_aiUpdate = nullptr, g_aiTick = nullptr;  // the game's functions, run by the step
bool               g_aiPlaced = false;
std::atomic<void*> g_eventAiController{nullptr};  // the member's field AI controller while its event runs (any thread)
std::atomic<std::uint32_t> g_aiSkipped{0};  // the game's runs skipped for it
std::uint32_t              g_aiRunInStep = 0;
bool                       g_aiFirstStep = false;  // the first step after the publish leaves the AI alone: a worker's run may still be in it

// Game thread.
std::array<SeatState, kSeats> g_seats{};
Event                         g_event;
std::uint32_t                 g_watchdogs = 0, g_deferredRun = 0;
platform::GameThreadMark      g_gameThread;

// Any thread.
std::atomic<void*>                     g_eventObject{nullptr};  // g_event.object, for the notify's quick test off the game thread
std::atomic<bool>                      g_inStep{false};         // the seat's step runs now: a notify inside it runs at once
std::atomic<std::uint32_t>             g_notifyOnThread{0}, g_notifyOffThread{0}, g_deferredDropped{0};
std::mutex                             g_deferredLock;
std::array<Deferred, kDeferredMax>     g_deferred{};
std::size_t                            g_deferredCount = 0;  // under g_deferredLock

std::uint8_t* Manager() noexcept
{
    return players::field_root::PushButtons();
}

/// The game's own test for "an event runs": the manager's active list has a head node.
bool ActiveListEmpty() noexcept
{
    std::uint8_t* manager = Manager();
    void*         head    = nullptr;
    return manager != nullptr && Read(manager + facts::kPushButtonsActiveHead, head) && head == nullptr;
}

/// `object` is on the manager's active list (a walk of its nodes, each naming a slot).
bool Active(const void* object) noexcept
{
    std::uint8_t* manager = Manager();
    std::uint8_t* node    = nullptr;
    if (manager == nullptr || !Read(manager + facts::kPushButtonsActiveHead, node))
    {
        return false;
    }
    for (int steps = 0; node != nullptr && steps < facts::kPushSlotCount; ++steps)
    {
        std::int32_t slot = -1;
        if (Read(node + facts::kPushNodeSlot, slot) && slot >= 0 && slot < facts::kPushSlotCount &&
            manager + facts::kPushButtonsObjects + static_cast<std::size_t>(slot) * facts::kPushObjStride == object)
        {
            return true;
        }
        if (!Read(node + facts::kPushNodeNext, node))
        {
            return false;
        }
    }
    return false;
}

/// A row's actor and action names as text, and the plan for `member` at it. False when the row cannot be read.
struct RowPlan
{
    push_event_rules::Plan plan;
    std::uint64_t          memberTag = 0;
    std::wstring           actor;  // the row's one actor, for the log
};

bool PlanAt(const std::uint8_t* row, const void* member, RowPlan& out) noexcept
try
{
    const std::uint64_t* actorNames  = static_cast<const std::uint64_t*>(ReadPointer(row, facts::kPushRowActorNames));
    const std::uint64_t* actionNames = static_cast<const std::uint64_t*>(ReadPointer(row, facts::kPushRowActions));
    std::int32_t         actorCount = 0, actionCount = 0;
    if (!Read(row + facts::kPushRowActorCount, actorCount) || !Read(row + facts::kPushRowActionCount, actionCount) || actorCount < 0 ||
        actionCount < 0 || actorCount > static_cast<std::int32_t>(kRowActors) || actionCount > static_cast<std::int32_t>(4 * kRowActors))
    {
        return false;
    }
    std::wstring      actors[kRowActors];
    std::wstring      actions[4 * kRowActors];
    std::wstring_view actorViews[kRowActors];
    std::wstring_view actionViews[4 * kRowActors];
    for (std::int32_t i = 0; i < actorCount; ++i)
    {
        std::uint64_t name = 0;
        if (actorNames == nullptr || !Read(actorNames + i, name))
        {
            return false;
        }
        actors[i]     = platform::NameText(name);
        actorViews[i] = actors[i];
    }
    for (std::int32_t i = 0; i < actionCount; ++i)
    {
        std::uint64_t name = 0;
        if (actionNames == nullptr || !Read(actionNames + i, name))
        {
            return false;
        }
        actions[i]     = name != 0 ? platform::NameText(name) : std::wstring();
        actionViews[i] = actions[i];
    }
    out.memberTag = field_targets::FirstTag(member);
    const std::wstring tag = out.memberTag != 0 ? platform::NameText(out.memberTag) : std::wstring();
    out.plan  = push_event_rules::PlanFor(actorViews, static_cast<std::size_t>(actorCount), actionViews, static_cast<std::size_t>(actionCount), tag);
    out.actor = actorCount > 0 ? actors[0] : std::wstring();
    return out.memberTag != 0;
}
catch (...)
{
    return false;
}

/// The push-button row of a target's ObjectLayout row, or null.
std::uint8_t* RowOfTarget(std::uint64_t targetName) noexcept
{
    const auto*   layout = static_cast<const std::uint8_t*>(field_targets::LayoutRow(targetName));
    std::uint64_t id     = 0;
    if (layout == nullptr || !Read(layout + facts::kLayoutPushButtonId, id) || id == 0)
    {
        return nullptr;
    }
    return static_cast<std::uint8_t*>(field_targets::RowIn(g_tablePush, id));
}

NotifyFn OriginalNotify() noexcept
{
    return reinterpret_cast<NotifyFn>(g_notify.Original());
}

/// The object is the running event of a seat's player, asked on the game thread (the step and the free run there).
bool OursOnGameThread(const void* object) noexcept
{
    return object != nullptr && object == g_event.object && g_gameThread.Here();
}

std::lock_guard<std::mutex> LockDeferred() noexcept
{
    return std::lock_guard<std::mutex>(g_deferredLock);
}

/// The event's step written as the end step: the game's own End runs at its next step (the member back on its controller,
/// the camera left, the object freed). The one way the mod ends an event. True when written.
bool SetEndStep(void* object) noexcept
{
    const std::int32_t end = facts::kPushStepEnd;
    return platform::WriteGuarded(static_cast<std::uint8_t*>(object) + facts::kPushObjStep, &end, sizeof(end));
}

/// The row's bytes exchanged for the length of the game's Init: its one actor name made the member's tag, and, with no
/// animation for the member, its action count made zero (every phase then plays nothing). Put back after: the game's
/// ctor copies every name by value inside Init, and the later steps read only the row's position fields and triggers
/// (the actor and action getters have no other caller; research/RE-further-player-levers.md).
class RowExchange
{
public:
    RowExchange(std::uint8_t* row, const RowPlan& plan) noexcept
    {
        if (plan.plan.asWritten)
        {
            return;
        }
        actorAt_ = static_cast<std::uint64_t*>(ReadPointer(row, facts::kPushRowActorNames));
        if (actorAt_ != nullptr && Read(actorAt_, actorWas_) && platform::WriteGuarded(actorAt_, &plan.memberTag, sizeof(plan.memberTag)))
        {
            actorSwapped_ = true;
        }
        else
        {
            actorAt_ = nullptr;
        }
        if (plan.plan.noAnimation)
        {
            countAt_ = reinterpret_cast<std::int32_t*>(row + facts::kPushRowActionCount);
            constexpr std::int32_t none = 0;
            countSwapped_ = Read(countAt_, countWas_) && platform::WriteGuarded(countAt_, &none, sizeof(none));
        }
    }
    ~RowExchange()
    {
        if (countSwapped_)
        {
            platform::WriteGuarded(countAt_, &countWas_, sizeof(countWas_));
        }
        if (actorSwapped_)
        {
            platform::WriteGuarded(actorAt_, &actorWas_, sizeof(actorWas_));
        }
    }
    RowExchange(const RowExchange&)            = delete;
    RowExchange& operator=(const RowExchange&) = delete;

    /// The exchange the plan asked for is in place (an exchange that could not be written refuses the event).
    bool Held(const RowPlan& plan) const noexcept
    {
        return plan.plan.asWritten || (actorSwapped_ && (!plan.plan.noAnimation || countSwapped_));
    }

private:
    std::uint64_t* actorAt_      = nullptr;
    std::uint64_t  actorWas_     = 0;
    bool           actorSwapped_ = false;
    std::int32_t*  countAt_      = nullptr;
    std::int32_t   countWas_     = 0;
    bool           countSwapped_ = false;
};

std::wstring Name(std::uint64_t bits) noexcept
try
{
    return field_targets::NameOf(bits);
}
catch (...)
{
    return L"?";
}

/// The member's field AI back to the game's own runs.
void ForgetMemberAi() noexcept
{
    g_eventAiController.store(nullptr, std::memory_order_release);
    g_aiFirstStep = false;
}

void DropEvent() noexcept
{
    g_event = Event{};
    g_eventObject.store(nullptr, std::memory_order_release);
    ForgetMemberAi();
    const auto lock = LockDeferred();
    g_deferredCount = 0;
}

/// One of the member's field AI functions as the game calls it, on whatever thread: skipped for the member whose event
/// runs (the step runs it, in the seat's scopes); the game's own run for every other controller.
void RunUnlessEvent(void* controller, float dt, AiFn original) noexcept
{
    if (controller != nullptr && controller == g_eventAiController.load(std::memory_order_acquire))
    {
        g_aiSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    original(controller, dt);
}

void AiUpdate(void* controller, float dt)
{
    RunUnlessEvent(controller, dt, g_aiUpdate);
}

void AiTick(void* controller, float dt)
{
    RunUnlessEvent(controller, dt, g_aiTick);
}

/// The member's field AI run once this frame, inside the seat's scopes: its actor tick, then its update, as the game's
/// tick groups order them. Not on the first step after the publish (a worker's run that passed the skip before it may
/// still be in the function), and not once the member no longer holds that controller (a level change: the pointer
/// is let go, the game's runs resume).
void RunMemberAi(float dt) noexcept
{
    void* controller = g_eventAiController.load(std::memory_order_acquire);
    if (controller == nullptr)
    {
        return;
    }
    if (g_aiFirstStep)
    {
        g_aiFirstStep = false;
        return;
    }
    if (players::party_member::FieldAiController(g_seats[g_event.seat].member) != controller)
    {
        ForgetMemberAi();
        return;
    }
    platform::CallGuarded(g_aiTick, controller, dt);
    platform::CallGuarded(g_aiUpdate, controller, dt);
    ++g_aiRunInStep;
}

// --- The redirects -----------------------------------------------------------------------------------------------------

/// The game's Init of a push-button event (the press of its target): for a seat's press (its press runs inside
/// its Leader scope), the row is planned for its member and exchanged around the call; the event is then that player's.
char Init(void* object, const std::uint64_t* targetName, const std::uint64_t* key, const std::uint64_t* actionId,
          const std::uint64_t* bgActorName, int slot)
{
    const auto original = reinterpret_cast<InitFn>(g_init.Original());
    const int  seat     = players::scopes::OpenPlayer(players::scopes::Leader);
    if (seat == 0 || !g_gameThread.Here() || actionId == nullptr || targetName == nullptr)
    {
        return original(object, targetName, key, actionId, bgActorName, slot);  // player 1's
    }
    std::uint64_t id = 0, target = 0;
    Read(actionId, id);
    Read(targetName, target);
    auto*   row    = static_cast<std::uint8_t*>(field_targets::RowIn(g_tablePush, id));
    RowPlan plan;
    if (row == nullptr || !PlanAt(row, players::scopes::Member(seat), plan) || !plan.plan.admitted)
    {
        // Not a row the player may work (the pick leaves such targets to player 1; a race with the row's data is possible):
        // refused, as the game refuses an event with no row; the game frees the object on the spot.
        ++g_seats[seat].pressesRefused;
        platform::LogWarning(L"levers: {}'s press of {} is not a lever a player may work (row {}); refused", SeatName(seat), Name(target), Name(id));
        return 0;
    }
    char result = 0;
    {
        const RowExchange exchange(row, plan);
        if (!exchange.Held(plan))
        {
            ++g_seats[seat].pressesRefused;
            platform::LogWarning(L"levers: the row {} could not be set for {}'s character; the press is refused", Name(id), SeatName(seat));
            return 0;
        }
        result = original(object, targetName, key, actionId, bgActorName, slot);
    }
    if (result == 0)
    {
        ++g_seats[seat].pressesRefused;
        platform::Log(L"levers: {}'s event {} (row {}) was refused by the game's start (no target entry or no icon)", SeatName(seat),
                      Name(target), Name(id));
        return 0;
    }
    g_event = Event{.object = object, .seat = seat, .actionId = id, .target = target, .noAnimation = plan.plan.noAnimation};
    void* aiController = g_aiPlaced ? players::party_member::FieldAiController(g_seats[seat].member) : nullptr;
    g_aiFirstStep      = aiController != nullptr;
    g_eventAiController.store(aiController, std::memory_order_release);
    if (g_aiPlaced && aiController == nullptr)
    {
        platform::LogWarning(L"levers: {}'s character has no field AI controller to run in the step; it may turn toward the leader during the hold",
                             SeatName(seat));
    }
    {
        const auto lock = LockDeferred();  // nothing kept from an earlier event in this slot
        g_deferredCount = 0;
        g_eventObject.store(object, std::memory_order_release);
    }
    ++g_seats[seat].events;
    platform::Log(L"levers: {} starts {} (row {}, authored for {}): {}", SeatName(seat), Name(target), Name(id), plan.actor,
                  plan.plan.asWritten ? L"as written, its own character's animation"
                                      : L"on its own character, with no pull animation (the character stands at it while the gauge fills)");
    return result;
}

/// A deferred notify run now, inside the seat's scopes, under the game's own lock as the game's dispatcher holds it.
void RunDeferred(void* object) noexcept
{
    std::array<Deferred, kDeferredMax> mine{};
    std::size_t                        count = 0;
    {
        const auto  lock = LockDeferred();
        std::size_t kept = 0;
        for (std::size_t i = 0; i < g_deferredCount; ++i)
        {
            if (g_deferred[i].object == object)
            {
                mine[count++] = g_deferred[i];
            }
            else
            {
                g_deferred[kept++] = g_deferred[i];
            }
        }
        g_deferredCount = kept;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        if (g_lock != nullptr)
        {
            EnterCriticalSection(g_lock);
        }
        std::uint64_t result = 0;
        platform::CallGuarded(OriginalNotify(), result, object, static_cast<void*>(nullptr), static_cast<const std::uint64_t*>(&mine[i].key));
        if (g_lock != nullptr)
        {
            LeaveCriticalSection(g_lock);
        }
        ++g_deferredRun;
        platform::LogDebug(L"levers: a finished-animation notify for {}'s event ran at its step ({})", SeatName(g_event.seat), Name(mine[i].key));
    }
}

const wchar_t* StepName(std::int32_t step) noexcept
{
    switch (step)
    {
        case facts::kPushStepLevelWait: return L"the level load";
        case facts::kPushStepPhaseZero: return L"the first animation";
        case facts::kPushStepHold: return L"the hold";
        case facts::kPushStepPress: return L"the press";
        case facts::kPushStepPhaseTwoWait: return L"the pull's end";
        case facts::kPushStepCancelWait: return L"the release's end";
        case facts::kPushStepEnd: return L"the end";
        case facts::kPushStepFreed: return L"done";
        default: return L"a step";
    }
}

/// After the seat's step: the step logged on change, and an event stuck in a wait ended (its end step set: the game's
/// own End runs next step, returns the member and frees the object).
void Watch(void* object, float dt) noexcept
{
    std::int32_t step = -1;
    if (!Read(static_cast<const std::uint8_t*>(object) + facts::kPushObjStep, step))
    {
        return;
    }
    if (step != g_event.lastStep)
    {
        g_event.lastStep    = step;
        g_event.stepSeconds = 0;
        platform::LogDebug(L"levers: {}'s event {}: step {} ({})", SeatName(g_event.seat), Name(g_event.target), step, StepName(step));
        return;
    }
    g_event.stepSeconds += dt;
    const bool  loadStuck   = step == facts::kPushStepLevelWait && g_event.stepSeconds > kLoadWaitSeconds;
    const bool  finishStuck = (step == facts::kPushStepPhaseZero || step == facts::kPushStepPhaseTwoWait || step == facts::kPushStepCancelWait) &&
                             g_event.stepSeconds > kFinishWaitSeconds;
    if ((loadStuck || finishStuck) && !g_event.ended)
    {
        g_event.ended = SetEndStep(object);
        ++g_watchdogs;
        platform::LogWarning(L"levers: {}'s event {} waited {:.1f} s in {}; ended by the watchdog{}", SeatName(g_event.seat), Name(g_event.target),
                             g_event.stepSeconds, StepName(step), g_event.ended ? L"" : L" (the end step could not be written)");
    }
}

/// The game's step of a push-button event, each frame while it runs: a seat's event steps inside its scopes with
/// its pad's hold and its own icons; any notify kept for it runs first.
void Step(void* object, float dt)
{
    const auto original = reinterpret_cast<StepFn>(g_step.Original());
    if (!OursOnGameThread(object))
    {
        original(object, dt);
        return;
    }
    const int        seat  = g_event.seat;
    const SeatState& state = g_seats[seat];
    if (const std::uint32_t dropped = g_deferredDropped.exchange(0, std::memory_order_relaxed); dropped != 0)
    {
        platform::LogWarning(L"levers: {} finished-animation notice(s) for {}'s event could not be kept (more than {} waiting); the "
                             L"watchdog ends a stalled wait",
                             dropped, SeatName(seat), kDeferredMax);
    }
    {
        const players::scopes::Scope            scope(seat, kScopes);
        const players::pad_buttons::ButtonScope buttons(static_cast<unsigned>(seat));  // the hold is read from the seat's pad
        if (!scope.Held() || !buttons.Held())
        {
            // Never stepped as player 1's (the event would move player 1's character): skipped this frame; a scope that stays
            // shut ends the event, the end step in the seat's own scopes being out of reach too.
            if (++g_event.unscopedFrames == 1)
            {
                platform::LogWarning(L"levers: {}'s scopes or pad could not be opened for its event's step; the step is skipped", SeatName(seat));
            }
            if (g_event.unscopedFrames >= kUnscopedFramesToEnd && !g_event.ended)
            {
                g_event.ended = SetEndStep(object);
                platform::LogWarning(L"levers: {}'s event {} could not be stepped in its scopes for {} frames; its end step is set", SeatName(seat),
                                     Name(g_event.target), g_event.unscopedFrames);
            }
            return;
        }
        g_event.unscopedFrames = 0;
        {
            RunDeferred(object);
            bool held = false;
            if (field_targets::ActionHeld(held))
            {
                const std::uint8_t byte = held ? 1 : 0;
                platform::WriteGuarded(static_cast<std::uint8_t*>(object) + facts::kPushObjHeld, &byte, 1);
            }
            const target_icons::SeatSwap icons(seat, players::field_root::FieldTargets(), state.localPlayer, false);
            g_inStep.store(true, std::memory_order_release);
            original(object, dt);
            g_inStep.store(false, std::memory_order_release);
            RunMemberAi(dt);
        }
    }
    Watch(object, dt);
}

/// The game's finished-animation notify for a push-button event: for a seat's event it runs at once inside that
/// event's step (the game's own case: a request that ended at once), else it is kept for the next step.
std::uint64_t Notify(void* object, void* unused, const std::uint64_t* key)
{
    if (object == nullptr || object != g_eventObject.load(std::memory_order_acquire) || key == nullptr)
    {
        return OriginalNotify()(object, unused, key);
    }
    const bool here = g_gameThread.Here();
    (here ? g_notifyOnThread : g_notifyOffThread).fetch_add(1, std::memory_order_relaxed);
    if (here && g_inStep.load(std::memory_order_acquire))
    {
        return OriginalNotify()(object, unused, key);
    }
    std::uint64_t copy = 0;
    if (!platform::CopyGuarded(&copy, key, sizeof(copy)))
    {
        return OriginalNotify()(object, unused, key);  // a key that cannot be read: the game's own call, as it is
    }
    const auto lock = LockDeferred();
    // Asked again under the lock: an event dropped meanwhile (its object's slot is reused by the next event) takes no
    // stale notify; the drop clears the object before it takes this lock.
    if (object != g_eventObject.load(std::memory_order_acquire))
    {
        return 1;
    }
    if (g_deferredCount < kDeferredMax)
    {
        g_deferred[g_deferredCount++] = Deferred{.object = object, .key = copy};
    }
    else
    {
        g_deferredDropped.fetch_add(1, std::memory_order_relaxed);  // logged by the step; the watchdog is the backstop
    }
    return 1;
}

/// The game's free of a done event (the field step, after the end step): a seat's runs inside its Operator scope
/// (the free resets a camera setting on the operator), and the event is forgotten.
void Free(void* object)
{
    const auto original = reinterpret_cast<FreeFn>(g_free.Original());
    if (!OursOnGameThread(object))
    {
        original(object);
        return;
    }
    const int seat = g_event.seat;
    {
        const players::scopes::Scope operatorScope(seat, players::scopes::Operator);  // the free resets a camera setting on it
        original(object);
    }
    platform::Log(L"levers: {}'s event {} ended ({})", SeatName(seat), Name(g_event.target),
                  g_event.ended                                    ? L"ended by the mod: the watchdog or the player's leave"
                  : g_event.lastStep == facts::kPushStepPress ||
                          g_event.lastStep == facts::kPushStepEnd ||
                          g_event.lastStep == facts::kPushStepFreed ? L"done: the target was pressed or the hold released"
                                                                    : L"freed by the game");
    DropEvent();
}

/// The member's field AI into the step: the four classes' two slots, all or none (a class left to the game would turn
/// its member away mid-hold). The game's functions are kept from before the first slot changes: a run that enters a
/// replaced slot reaches them at once, and keeps reaching them after a rollback or the removal.
void PlaceFieldAiSlots(const platform::GameImage& image) noexcept
{
    const auto update = platform::FindFunction(image, facts::kFieldAiUpdate);
    const auto tick   = platform::FindFunction(image, facts::kFieldAiTick);
    bool       placed = update && tick;
    if (placed)
    {
        g_aiUpdate = reinterpret_cast<AiFn>(*update);
        g_aiTick   = reinterpret_cast<AiFn>(*tick);
    }
    for (std::size_t i = 0; placed && i < facts::kFieldAiControllerVtables.size(); ++i)
    {
        const std::uintptr_t vtable = facts::kFieldAiControllerVtables[i];
        placed = platform::ReplaceSlot(image, {L"a field AI controller's update slot", vtable + facts::kFieldAiUpdateSlot, facts::kFieldAiUpdate},
                                       reinterpret_cast<std::uintptr_t>(&AiUpdate), g_aiSlots[2 * i]) &&
                 platform::ReplaceSlot(image, {L"a field AI controller's tick slot", vtable + facts::kFieldAiTickSlot, facts::kFieldAiTick},
                                       reinterpret_cast<std::uintptr_t>(&AiTick), g_aiSlots[2 * i + 1]);
    }
    g_aiPlaced = placed;
    if (!placed)
    {
        for (platform::VTableEntry& slot : g_aiSlots)
        {
            slot.Restore();
        }
        platform::LogWarning(L"levers: the field AI's slots are not as the research read them; a character without the lever animation may "
                             L"turn toward the leader during its hold and lose the lever");
    }
}

std::wstring Report()
{
    std::wstring out = std::format(L"{}; notifies on the game thread {}, elsewhere {} (deferred and run {}); watchdog ends {}; the member's field "
                                   L"AI {}: runs in the step {}, the game's skipped {}",
                                   !g_installed ? L"not installed" : g_init.IsInstalled() ? L"on" : L"off (a call could not be redirected)",
                                   g_notifyOnThread.load(), g_notifyOffThread.load(), g_deferredRun, g_watchdogs,
                                   g_aiPlaced ? L"in the step while an event runs" : L"the game's (its slots not taken)", g_aiRunInStep,
                                   g_aiSkipped.load());
    if (g_event.object != nullptr)
    {
        out += std::format(L"; running: {}'s {} at step {} for {:.1f} s{}", SeatName(g_event.seat), Name(g_event.target), g_event.lastStep,
                           g_event.stepSeconds, g_event.noAnimation ? L" (no animation)" : L"");
    }
    else
    {
        out += std::format(L"; no event of players 2 or 3; the game's active list {}", ActiveListEmpty() ? L"empty" : L"has an event");
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        out += std::format(L"; {}: {} event(s), {} press(es) refused", SeatName(seat), g_seats[seat].events, g_seats[seat].pressesRefused);
    }
    return out;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto fail = [](const wchar_t* what) {
        platform::LogWarning(L"levers: {} is not as the research read it; players 2 and 3 press no levers", what);
        return false;
    };
    const auto initSite   = image.Rebase(facts::kPushInitCall, 5);
    const auto stepSite   = image.Rebase(facts::kPushStepCall, 5);
    const auto notifySite = image.Rebase(facts::kPushNotifyCall, 5);
    const auto freeSite   = image.Rebase(facts::kPushFreeCall, 5);
    const auto init       = platform::FindFunction(image, facts::kPushInit);
    const auto step       = platform::FindFunction(image, facts::kPushStep);
    const auto notify     = platform::FindFunction(image, facts::kPushNotify);
    const auto freeFn     = platform::FindFunction(image, facts::kPushFree);
    const auto push       = image.Rebase(facts::kTablePushButtonAction, 24);
    const auto lock       = image.Rebase(facts::kPushButtonsLock, sizeof(CRITICAL_SECTION));
    if (!players::field_root::Installed() || !initSite || !stepSite || !notifySite || !freeSite || !init || !step || !notify || !freeFn ||
        !push || !lock)
    {
        return fail(L"a function, call site, table or lock");
    }
    g_tablePush = *push;
    g_lock      = reinterpret_cast<CRITICAL_SECTION*>(*lock);
    // All four or none: an event started as a player's but stepped as player 1's would walk player 1's character.
    const bool placed = g_init.Install(*initSite, *init, reinterpret_cast<std::uintptr_t>(&Init)) &&
                        g_step.Install(*stepSite, *step, reinterpret_cast<std::uintptr_t>(&Step)) &&
                        g_notify.Install(*notifySite, *notify, reinterpret_cast<std::uintptr_t>(&Notify)) &&
                        g_free.Install(*freeSite, *freeFn, reinterpret_cast<std::uintptr_t>(&Free));
    if (!placed)
    {
        g_free.Restore();
        g_notify.Restore();
        g_step.Restore();
        g_init.Restore();
        return fail(L"one of the four calls");
    }
    g_installed = true;
    PlaceFieldAiSlots(image);
    control::Register({.name  = L"levers",
                       .shape = control::Shape::Text,
                       .help  = L"the lever or switch event player 2 or 3 runs, its step, and the counts",
                       .get   = [] { return control::Value{Reading()}; }});
    platform::Log(L"levers: installed; players 2 and 3 work the levers and switches authored for one character, on their own character");
    return true;
}

void Remove() noexcept
{
    g_installed = false;
    g_aiPlaced  = false;
    for (platform::VTableEntry& slot : g_aiSlots)
    {
        slot.Restore();
    }
    g_free.Restore();
    g_notify.Restore();
    g_step.Restore();
    g_init.Restore();
    DropEvent();
    for (SeatState& state : g_seats)
    {
        state.localPlayer = state.member = nullptr;
    }
}

void TickSeat(int seat, void* localPlayer, void* member) noexcept
{
    if (!g_installed || !players::SeatBuilt(seat))
    {
        return;
    }
    g_seats[seat].localPlayer = localPlayer;
    g_seats[seat].member      = member;
}

void Tick() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_gameThread.Set();
    // An event the game dropped without its free (a level change emptied the manager): forgotten.
    if (g_event.object != nullptr && !Active(g_event.object))
    {
        platform::Log(L"levers: {}'s event {} left the game's list without its free (a level change); forgotten", SeatName(g_event.seat),
                      Name(g_event.target));
        DropEvent();
    }
}

bool LeverRow(std::uint64_t name, const void* member) noexcept
{
    if (!g_installed || member == nullptr)
    {
        return false;
    }
    const std::uint8_t* row = RowOfTarget(name);
    RowPlan             plan;
    return row != nullptr && PlanAt(row, member, plan) && plan.plan.admitted;
}

bool Admitted(int seat) noexcept
{
    if (!g_installed || !g_init.IsInstalled() || !players::SeatBuilt(seat) || !ActiveListEmpty())
    {
        return false;
    }
    const void*  member = g_seats[seat].member;
    const auto*  action = players::actor_fields::FieldAction(member);
    std::uint8_t running = 0;
    return member != nullptr && players::party_member::ActorMode(member) != facts::kActorModeBattle && action != nullptr &&
           Read(action + facts::kFieldActionState, running) && running == 0;
}

bool Running(int seat, std::uint64_t name) noexcept
{
    return g_gameThread.Here() && g_event.object != nullptr && g_event.seat == seat && g_event.target == name;
}

void OnSeatLeaving(int seat) noexcept
{
    if (g_event.object == nullptr || g_event.seat != seat || !g_gameThread.Here())
    {
        return;
    }
    // The end step set and stepped once inside the seat's scopes: the game's End returns the member to the seat's controller
    // and leaves the camera; the field step then frees the object (as the game's, the seat being gone).
    bool stepped = false;
    if (SetEndStep(g_event.object))
    {
        const players::scopes::Scope scope(seat, kScopes);
        if (scope.Held())
        {
            stepped = platform::CallGuarded(reinterpret_cast<StepFn>(g_step.Original()), g_event.object, 0.0f);
        }
    }
    platform::Log(L"levers: {} leaves during its event {}: ended {}", SeatName(seat), Name(g_event.target),
                  stepped ? L"(its end step run in the player's scopes)" : L"as far as the end step could be set (the step did not run)");
    DropEvent();
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

}  // namespace lss::world::push_events
