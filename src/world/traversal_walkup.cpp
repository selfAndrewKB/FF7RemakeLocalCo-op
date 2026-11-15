#include "world/traversal_walkup.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/field_root.h"
#include "players/game_globals.h"
#include "players/party_member.h"
#include "players/player_trigger.h"
#include "players/seat_camera.h"
#include "players/scopes.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "world/actor_place.h"
#include "world/field_targets.h"
#include "world/traversal_action.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace lss::world::traversal_walkup {

namespace {

using platform::Read;
using platform::ReadPointer;
using players::weak_object::Weak;

using SweepFn      = void (*)(void* controller);
using CellFn       = void* (*)();
using RouterFn     = char (*)(std::uint64_t volume, void* collider);
using NaviAreaFn   = std::uint64_t (*)(void* naviManager, void* volume);
using NaviRemoveFn = std::uint64_t (*)(void* list, void* volume, std::uint64_t third);  // r8 as the game passes it
using MgrGetFn     = void* (*)();
using NameFindFn   = void* (*)(void* map, std::int32_t* index, std::uint64_t key);
using TryStartFn   = void (*)(void* actor, float dt);
using MarkersFn    = void (*)(void* actor, float dt, const float* point);

constexpr std::size_t kClasses    = std::size(facts::kWalkUpClasses);
constexpr int         kSeats      = players::scopes::kMaxPlayers;
constexpr std::size_t kShadows    = 16;   // spots a seat's trigger can be inside at once (two or three in practice)
constexpr std::size_t kQueue      = 64;   // PlayerTrigger entries pending between a sweep and the dispatch
constexpr std::size_t kBlockA     = 0x38;
constexpr std::size_t kBlockB     = 0x30;
constexpr std::size_t kCellZero   = facts::kControllerTriggerComponent + 8;

/// Why a seat's start test at a spot did not start its action (said once per visit).
enum class Held : std::uint8_t
{
    None,
    OtherUser,
    IgnoreGroup,
    WithAi,
    NoComponent,
    NoScope,
    NoActionSide,
    GameTest,
};

/// A seat's copy of one actor's user block.
struct Shadow
{
    void*                          actor = nullptr;
    Weak                           weak;  // the actor, to tell a reused address
    std::size_t                    cls   = 0;
    std::array<std::uint8_t, kBlockA> a{};
    std::array<std::uint8_t, kBlockB> b{};
    Held                           said     = Held::None;  // the last wait logged for this visit; an enter clears it
    bool                           liftSaid = false;       // the lifted ignore group logged for this visit
};

struct SeatState
{
    void*                            controller = nullptr;
    void*                            trigger    = nullptr;  // its trigger's overlap component: a new one starts an empty set
    void*                            member     = nullptr;
    std::array<Shadow, kShadows>     shadows{};
    std::uint32_t                    enters = 0, leaves = 0, heldBack = 0, requests = 0;
};

/// One PlayerTrigger entry of the pending list: whose trigger added it (0 player 1) and that seat's controller then.
struct Queued
{
    int   seat       = 0;
    void* controller = nullptr;
};

// Set at install, then only read.
SweepFn      g_sweepOriginal = nullptr;
CellFn       g_partyManagerGet = nullptr;
MgrGetFn     g_fieldActionMgr  = nullptr;
NameFindFn   g_nameFind        = nullptr;
std::uintptr_t g_pendingNames  = 0;
std::uint64_t  g_playerTrigger = 0;
std::uintptr_t g_partyManager  = 0;
bool           g_installed     = false;

platform::VTableEntry                 g_sweep;
platform::CallRedirect                g_resolve, g_begin, g_end, g_naviList, g_naviEnter, g_naviRemove, g_naviLeave;
std::array<platform::VTableEntry, kClasses> g_tryStart, g_markers;

// Game thread.
std::array<SeatState, kSeats> g_seats{};
std::array<Queued, kQueue>    g_queue{};
std::size_t                   g_queueHead = 0, g_queueCount = 0;
int                           g_current   = 0;  // the seat whose entry the dispatcher is routing now (0 player 1)
bool                          g_delivering = false;
std::array<void*, kSeats>     g_cells{};        // a seat's controller, as the party manager's +0 for one resolve
std::array<std::uint8_t, kCellZero> g_zero{};   // a "controller" with no trigger component: the dispatcher skips it
void*                         g_zeroCell = g_zero.data();

void* PrimaryController() noexcept
{
    return ReadPointer(ReadPointer(reinterpret_cast<const void*>(g_partyManager), 0), 0);
}

std::int32_t PendingCount() noexcept
{
    std::int32_t count = 0;
    Read(reinterpret_cast<const void*>(g_pendingNames + 8), count);
    return count;
}

bool PendingIsPlayerTrigger(std::int32_t index) noexcept
{
    const auto* data = static_cast<const std::uint8_t*>(ReadPointer(reinterpret_cast<const void*>(g_pendingNames), 0));
    std::uint64_t name = 0;
    return index >= 0 && data != nullptr && Read(data + 8 * static_cast<std::size_t>(index), name) && name == g_playerTrigger;
}

/// The PlayerTrigger entries the pending list holds now (each one resolve of the dispatcher's).
std::size_t PendingPlayerTriggers() noexcept
{
    const std::int32_t count = PendingCount();
    std::size_t        found = 0;
    for (std::int32_t i = 0; i < count && i < 4096; ++i)
    {
        found += PendingIsPlayerTrigger(i) ? 1 : 0;
    }
    return found;
}

void Push(int seat, void* controller) noexcept
{
    if (g_queueCount == kQueue)
    {
        platform::LogWarning(L"walk-up: the trigger queue overflowed; its oldest entry is dropped");
        g_queueHead = (g_queueHead + 1) % kQueue;
        --g_queueCount;
    }
    g_queue[(g_queueHead + g_queueCount) % kQueue] = {seat, controller};
    ++g_queueCount;
}

/// The queue in step with the list: one item per PlayerTrigger entry still pending, the newest kept (the oldest are
/// the ones a missed dispatch left), and player 1 for an entry the queue does not know.
void Resync() noexcept
{
    const std::size_t pending = PendingPlayerTriggers();
    if (pending == g_queueCount)
    {
        return;
    }
    platform::LogDebug(L"walk-up: the trigger queue had {} entr(ies) for {} pending; put in step", g_queueCount, pending);
    while (g_queueCount > pending)
    {
        g_queueHead = (g_queueHead + 1) % kQueue;
        --g_queueCount;
    }
    while (g_queueCount < pending && g_queueCount < kQueue)
    {
        g_queueHead          = (g_queueHead + kQueue - 1) % kQueue;
        g_queue[g_queueHead] = {};  // player 1's
        ++g_queueCount;
    }
}

/// The game's sweep of one controller's trigger, and its entry queued if it added one.
void SweepOne(int seat, void* controller) noexcept
{
    const std::int32_t before = PendingCount();
    g_sweepOriginal(controller);
    const std::int32_t after = PendingCount();
    if (after == before + 1 && PendingIsPlayerTrigger(after - 1))
    {
        Push(seat, controller);
    }
}

void Sweep(void* controller)
{
    if (controller != PrimaryController())
    {
        g_sweepOriginal(controller);
        return;
    }
    g_current = 0;
    Resync();
    SweepOne(0, controller);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        void* own = g_seats[seat].controller;
        if (own != nullptr && ReadPointer(own, facts::kControllerTriggerComponent) != nullptr)
        {
            SweepOne(seat, own);
        }
    }
}

/// The dispatcher's resolve of a PlayerTrigger entry: the controller whose trigger added it, as the party manager's +0.
void* Resolve()
{
    if (g_queueCount == 0)
    {
        return g_partyManagerGet();
    }
    const Queued entry = g_queue[g_queueHead];
    g_queueHead        = (g_queueHead + 1) % kQueue;
    --g_queueCount;
    if (entry.seat == 0)
    {
        return g_partyManagerGet();
    }
    if (entry.controller == nullptr || entry.controller != g_seats[entry.seat].controller)
    {
        return &g_zeroCell;  // the seat left after its sweep: the entry is skipped
    }
    g_cells[entry.seat] = entry.controller;
    return &g_cells[entry.seat];
}

int SeatOfCollider(const void* collider) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::player_trigger::OwnedBy(seat, collider))
        {
            return seat;
        }
    }
    return 0;
}

void* ActorOfVolume(std::uint64_t volume) noexcept
{
    auto* manager = static_cast<std::uint8_t*>(g_fieldActionMgr());
    if (manager == nullptr)
    {
        return nullptr;
    }
    std::int32_t index = -1;
    g_nameFind(manager + facts::kFieldActionMgrVolumes, &index, volume);
    if (index < 0)
    {
        return nullptr;
    }
    const auto* pairs = static_cast<const std::uint8_t*>(ReadPointer(manager, facts::kFieldActionMgrVolumes));
    return pairs == nullptr ? nullptr
                            : ReadPointer(pairs + static_cast<std::size_t>(index) * facts::kNameMapPairSize, facts::kNameMapPairValue);
}

// The live vtables (rebased at install).
std::array<std::uintptr_t, kClasses> g_vtables{};
std::array<bool, kClasses>           g_admitted{};  // a class a seat's trigger reaches (set at install)
static_assert(std::wstring_view(facts::kWalkUpClasses[facts::kWalkUpPole].name) == L"pole slide");

/// The admitted class of a live actor (its native vtable: placed Blueprint actors keep it), or kClasses.
std::size_t ClassOfVtable(const void* actor) noexcept
{
    const auto vtable = reinterpret_cast<std::uintptr_t>(ReadPointer(actor, 0));
    for (std::size_t cls = 0; cls < kClasses; ++cls)
    {
        if (vtable != 0 && vtable == g_vtables[cls])
        {
            return cls;
        }
    }
    return kClasses;
}

/// The seat's copy for a live actor (made on an enter), or null. A copy whose actor is gone (streamed out while the
/// seat was inside: no leave comes) counts as free.
Shadow* FindShadow(int seat, void* actor, bool create, std::size_t cls) noexcept
{
    SeatState& state = g_seats[seat];
    if (state.controller == nullptr)
    {
        return nullptr;
    }
    Shadow* free = nullptr;
    for (Shadow& shadow : state.shadows)
    {
        const bool gone = shadow.actor != nullptr && players::weak_object::Resolve(shadow.weak) != shadow.actor;
        if (gone)
        {
            shadow = Shadow{};
        }
        if (shadow.actor == actor && actor != nullptr)
        {
            return &shadow;  // the same live object: its weak pair still resolves to this address
        }
        if (shadow.actor == nullptr && free == nullptr)
        {
            free = &shadow;
        }
    }
    if (!create)
    {
        return nullptr;
    }
    const Weak weak = players::weak_object::MakeWeak(actor);
    if (!weak.Set())
    {
        platform::LogDebug(L"walk-up: {} could not keep a spot ({:#x}: no weak pair)", players::SeatName(seat), reinterpret_cast<std::uintptr_t>(actor));
        return nullptr;
    }
    if (free == nullptr)
    {
        platform::LogWarning(L"walk-up: {} is inside more than {} spots; the first copy is dropped", players::SeatName(seat), kShadows);
        free = &state.shadows[0];
    }
    *free       = Shadow{};
    free->actor = actor;
    free->weak  = weak;
    free->cls   = cls;
    const Weak none;  // {-1, 0}: no user yet
    std::memcpy(free->a.data(), &none, sizeof(none));
    return free;
}

/// The actor's real user block and the seat's copy exchanged (twice puts both back).
void Exchange(void* actor, Shadow& shadow) noexcept
{
    const auto& cls  = facts::kWalkUpClasses[shadow.cls];
    auto*       base = static_cast<std::uint8_t*>(actor);
    std::array<std::uint8_t, kBlockA> a{};
    std::array<std::uint8_t, kBlockB> b{};
    platform::CopyGuarded(a.data(), base + cls.blockA, cls.sizeA);
    platform::WriteGuarded(base + cls.blockA, shadow.a.data(), cls.sizeA);
    std::memcpy(shadow.a.data(), a.data(), cls.sizeA);
    if (cls.sizeB != 0)
    {
        platform::CopyGuarded(b.data(), base + cls.blockB, cls.sizeB);
        platform::WriteGuarded(base + cls.blockB, shadow.b.data(), cls.sizeB);
        std::memcpy(shadow.b.data(), b.data(), cls.sizeB);
    }
}

Weak UserOf(const Shadow& shadow) noexcept
{
    Weak weak;
    std::memcpy(&weak, shadow.a.data(), sizeof(weak));
    return weak;
}

/// A seat's enter or leave delivered to the game's router for an admitted spot, with the seat's member as the leader and
/// the seat's copy of the spot's user block in place.
char Deliver(int seat, void* actor, std::size_t cls, bool enter, std::uint64_t volume, void* collider, RouterFn router)
{
    if (g_delivering)
    {
        platform::LogWarning(L"walk-up: a nested delivery for {} was refused", players::SeatName(seat));
        return 0;
    }
    const bool fresh  = enter && FindShadow(seat, actor, false, cls) == nullptr;
    Shadow*    shadow = FindShadow(seat, actor, enter, cls);
    if (shadow == nullptr)
    {
        return 0;  // a leave of a spot this seat holds no copy of: nothing of the seat's to clear
    }
    g_delivering = true;
    Exchange(actor, *shadow);
    char result = 0;
    bool held   = false;
    {
        const players::scopes::Scope scope(seat, players::scopes::Leader);
        held = scope.Held();
        if (held)
        {
            result = router(volume, collider);
        }
    }
    Exchange(actor, *shadow);  // the seat's block back into its copy, the real one back on the actor
    g_delivering = false;
    SeatState& state = g_seats[seat];
    if (!held)
    {
        if (fresh)
        {
            *shadow = Shadow{};
        }
        platform::LogDebug(L"walk-up: {}'s {} of a {} not delivered (no member bound)", players::SeatName(seat), enter ? L"enter" : L"leave",
                           facts::kWalkUpClasses[cls].name);
        return 0;
    }
    if (enter)
    {
        ++state.enters;
        shadow->said     = Held::None;  // a new visit says its wait again
        shadow->liftSaid = false;
        platform::LogDebug(L"walk-up: {} is at a {} ({:#x})", players::SeatName(seat), facts::kWalkUpClasses[cls].name,
                           reinterpret_cast<std::uintptr_t>(actor));
    }
    else
    {
        ++state.leaves;
        platform::LogDebug(L"walk-up: {} left a {} ({:#x})", players::SeatName(seat), facts::kWalkUpClasses[cls].name,
                           reinterpret_cast<std::uintptr_t>(actor));
        if (!UserOf(*shadow).Set())
        {
            *shadow = Shadow{};  // the leave cleared the seat's user: the copy is done
        }
    }
    return result;
}

char Route(std::uint64_t volume, void* collider, bool enter, const platform::CallRedirect& redirect)
{
    const auto router = reinterpret_cast<RouterFn>(redirect.Original());
    g_current         = SeatOfCollider(collider);
    if (g_current == 0)
    {
        return router(volume, collider);
    }
    void*             actor = ActorOfVolume(volume);
    const std::size_t cls   = actor == nullptr ? kClasses : ClassOfVtable(actor);
    if (cls == kClasses || !g_admitted[cls])
    {
        ++g_seats[g_current].heldBack;
        if (actor == nullptr)
        {
            platform::LogDebug(L"walk-up: {}'s trigger {} volume {:#x} held back (not a field-action spot)", players::SeatName(g_current),
                               enter ? L"entered" : L"left", volume);
            return 0;
        }
        const auto vtable = reinterpret_cast<std::uintptr_t>(ReadPointer(actor, 0));
        platform::LogDebug(L"walk-up: {}'s trigger {} volume {:#x} held back (a field-action spot of a kind not admitted yet, vtable "
                           L"{})", players::SeatName(g_current), enter ? L"entered" : L"left", volume,
                           vtable != 0 ? std::format(L"{:#x} in IDA", platform::IdaAddress(vtable)) : std::wstring(L"unreadable"));
        return 0;
    }
    return Deliver(g_current, actor, cls, enter, volume, collider, router);
}

char Begin(std::uint64_t volume, void* collider)
{
    return Route(volume, collider, true, g_begin);
}

char End(std::uint64_t volume, void* collider)
{
    return Route(volume, collider, false, g_end);
}

/// The dispatcher's navi-volume list: the routed seat's own controller's.
void* NaviList()
{
    if (g_current != 0 && g_seats[g_current].controller != nullptr)
    {
        g_cells[g_current] = g_seats[g_current].controller;
        return &g_cells[g_current];
    }
    return reinterpret_cast<CellFn>(g_naviList.Original())();
}

std::uint64_t NaviRemove(void* list, void* volume, std::uint64_t third)
{
    if (g_current != 0 && g_seats[g_current].controller != nullptr)
    {
        list = static_cast<std::uint8_t*>(g_seats[g_current].controller) + facts::kControllerNaviVolumes;
    }
    return reinterpret_cast<NaviRemoveFn>(g_naviRemove.Original())(list, volume, third);
}

std::uint64_t NaviEnter(void* manager, void* volume)
{
    return g_current != 0 ? 0 : reinterpret_cast<NaviAreaFn>(g_naviEnter.Original())(manager, volume);
}

std::uint64_t NaviLeave(void* manager, void* volume)
{
    return g_current != 0 ? 0 : reinterpret_cast<NaviAreaFn>(g_naviLeave.Original())(manager, volume);
}

bool InIgnoreGroup(const void* actor, const void* member) noexcept
{
    const std::uint64_t tag = world::field_targets::FirstTag(member);
    if (tag == 0)
    {
        return false;
    }
    const auto*  names = static_cast<const std::uint8_t*>(ReadPointer(actor, facts::kFieldActionIgnore));
    std::int32_t count = 0;
    Read(static_cast<const std::uint8_t*>(actor) + facts::kFieldActionIgnore + 8, count);
    for (std::int32_t i = 0; names != nullptr && i < count && i < 64; ++i)
    {
        std::uint64_t name = 0;
        if (Read(names + 8 * static_cast<std::size_t>(i), name) && name == tag)
        {
            return true;
        }
    }
    return false;
}

/// The spot ignore groups players 2 and 3 are let through: Wall Market's alley squeezes and crawls, whose group keeps
/// the companions at the mouth while Cloud ducks in for a chest (it names exactly those ten spots, none of them the
/// story's); a player playing a listed companion may follow him in. The game's own accept, which also reads the group,
/// does not bind a seat's member while no field lock is set (research/RE-walkup-ignore-groups.md).
constexpr const wchar_t* kGroupsLiftedForPlayers[] = {L"agp080_SLU5B_40_PartyMember"};

/// The spot's ignore group is one players are let through.
bool GroupLifted(const void* actor) noexcept
{
    std::uint64_t row = 0;
    if (!Read(static_cast<const std::uint8_t*>(actor) + facts::kFieldActionIgnoreRow, row) || row == 0)
    {
        return false;
    }
    for (const wchar_t* group : kGroupsLiftedForPlayers)
    {
        if (platform::FindName(group) == row)
        {
            return true;
        }
    }
    return false;
}

template <class T>
struct Kept
{
    Kept()                       = default;
    Kept(const Kept&)            = delete;
    Kept& operator=(const Kept&) = delete;
    T*   at   = nullptr;
    T    value{};
    bool kept = false;
    void Hold(T* address, T replacement) noexcept
    {
        at   = address;
        kept = address != nullptr && Read(address, value) && platform::WriteGuarded(address, &replacement, sizeof(T));
    }
    ~Kept()
    {
        if (kept)
        {
            platform::WriteGuarded(at, &value, sizeof(T));
        }
    }
};

/// Why a seat at a spot has not started its action yet, logged once per visit and wait.
void SayHeld(int seat, Shadow& shadow, const void* actor, Held why) noexcept
{
    if (shadow.said == why)
    {
        return;
    }
    shadow.said = why;
    const wchar_t* text = L"";
    switch (why)
    {
    case Held::None: return;
    case Held::OtherUser: text = L"the spot's copy names another character than the seat's member"; break;
    case Held::IgnoreGroup: text = L"the member is in the spot's ignore group"; break;
    case Held::WithAi: text = L"the member is with its AI"; break;
    case Held::NoComponent: text = L"the member has no field-action component"; break;
    case Held::NoScope: text = L"the seat's leader scope could not be held"; break;
    case Held::NoActionSide: text = L"the field actions do not run in the seats' scopes yet"; break;
    case Held::GameTest: text = L"the game's start test (speed, heading, a busy member or a field gate)"; break;
    }
    platform::LogDebug(L"walk-up: {}'s member waits at a {} ({:#x}): {}", players::SeatName(seat),
                       facts::kWalkUpClasses[shadow.cls].name, reinterpret_cast<std::uintptr_t>(actor), text);
}

/// One seat's run of the game's TryStart on a spot whose copy names the seat's member.
void TryStartFor(int seat, void* actor, std::size_t cls, float dt, TryStartFn original) noexcept
{
    SeatState& state  = g_seats[seat];
    Shadow*    shadow = FindShadow(seat, actor, false, cls);
    if (shadow == nullptr || state.member == nullptr)
    {
        return;  // not at this spot
    }
    if (players::weak_object::Resolve(UserOf(*shadow)) != state.member)
    {
        SayHeld(seat, *shadow, actor, Held::OtherUser);
        return;
    }
    if (InIgnoreGroup(actor, state.member))
    {
        if (!GroupLifted(actor))
        {
            SayHeld(seat, *shadow, actor, Held::IgnoreGroup);
            return;
        }
        if (!std::exchange(shadow->liftSaid, true))
        {
            platform::LogDebug(L"walk-up: {}'s member may take this {} ({:#x}): its ignore group is lifted for players", players::SeatName(seat),
                               facts::kWalkUpClasses[cls].name, reinterpret_cast<std::uintptr_t>(actor));
        }
    }
    if (players::party_member::HandedToAi(state.member))
    {
        SayHeld(seat, *shadow, actor, Held::WithAi);
        return;  // with its AI (a lead, a scene): the game's member, not the player's (world/traversal_action.h)
    }
    auto*       base   = static_cast<std::uint8_t*>(actor);
    const auto& spot   = facts::kWalkUpClasses[cls];
    auto*       root   = players::field_root::Root();
    auto*       comp   = players::actor_fields::FieldAction(state.member);
    if (comp == nullptr)
    {
        SayHeld(seat, *shadow, actor, Held::NoComponent);
        return;  // a member without a field-action component cannot take a spot
    }
    std::uint8_t pendingBefore = 0;
    Read(comp + facts::kFieldActionPending, pendingBefore);

    bool ran = false;
    Exchange(actor, *shadow);
    {
        Kept<std::uint8_t> exception, latch;
        Kept<float>        cooldown;
        if (spot.exceptionFlag != 0)
        {
            exception.Hold(base + spot.exceptionFlag, 0);  // the member branch would take player 1's leader in this scope
        }
        if (cls == facts::kWalkUpCrawl)
        {
            float value = 0;
            Read(base + facts::kCrawlCooldown, value);
            cooldown.Hold(reinterpret_cast<float*>(base + facts::kCrawlCooldown), value);  // counted down once a frame, by player 1's run
        }
        if (root != nullptr)
        {
            latch.Hold(root + facts::kRootStickLatch, 0);  // player 1's camera-cut latch: the seat's own camera decides
        }
        // "No new action" without player 1's own action's share and with the seat's own (a script's hold stays): opened
        // before the seat's Leader scope below, inside which the party leader is the seat's member.
        const traversal_action::NoActionScope noAction(seat, true);
        const players::scopes::Scope scope(seat, players::scopes::Leader | players::scopes::Primary);
        ran = scope.Held();
        if (ran)
        {
            const players::seat_camera::StickScope stick(static_cast<std::uint32_t>(seat), true);
            original(actor, dt);
        }
    }
    Exchange(actor, *shadow);

    std::uint8_t pendingAfter = 0;
    Read(comp + facts::kFieldActionPending, pendingAfter);
    platform::LogTrace(L"walk-up: {}'s start test at a {} ({:#x})", players::SeatName(seat), spot.name, reinterpret_cast<std::uintptr_t>(actor));
    if (pendingAfter != 0 && pendingBefore == 0)
    {
        ++state.requests;
        shadow->said = Held::None;
        platform::LogDebug(L"walk-up: {}'s member starts the {} ({:#x})", players::SeatName(seat), spot.name, reinterpret_cast<std::uintptr_t>(actor));
    }
    else if (pendingBefore == 0)
    {
        SayHeld(seat, *shadow, actor, ran ? Held::GameTest : Held::NoScope);
    }
}

template <std::size_t Cls>
void TryStart(void* actor, float dt)
{
    const auto original = reinterpret_cast<TryStartFn>(g_tryStart[Cls].Original());
    original(actor, dt);  // player 1's, the member branch included
    if (!traversal_action::Scoping())
    {
        // A seat's start would run the game's member version (no pad, no camera): wait for the action side.
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            if (Shadow* shadow = FindShadow(seat, actor, false, Cls); shadow != nullptr)
            {
                SayHeld(seat, *shadow, actor, Held::NoActionSide);
            }
        }
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        TryStartFor(seat, actor, Cls, dt, original);
    }
}

bool LocationOf(const void* actor, float (&out)[3]) noexcept
{
    return actor_place::ActorLocation(actor, out);  // the markers' arithmetic takes arrays
}

template <std::size_t Cls>
void Markers(void* actor, float dt, const float* point)
{
    const auto original = reinterpret_cast<MarkersFn>(g_markers[Cls].Original());
    float      spot[3]{};
    float      best[3]{point[0], point[1], point[2]};
    if (LocationOf(actor, spot))
    {
        const auto distance = [&](const float (&at)[3]) {
            return std::hypot(at[0] - spot[0], at[1] - spot[1], at[2] - spot[2]);
        };
        float nearest = distance(best);
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            float at[3]{};
            if (g_seats[seat].member != nullptr && LocationOf(g_seats[seat].member, at) && distance(at) < nearest)
            {
                nearest = distance(at);
                std::memcpy(best, at, sizeof(best));
            }
        }
    }
    original(actor, dt, best);
}

template <std::size_t... I>
std::array<std::uintptr_t, kClasses> TryStarts(std::index_sequence<I...>) noexcept
{
    return {reinterpret_cast<std::uintptr_t>(&TryStart<I>)...};
}

template <std::size_t... I>
std::array<std::uintptr_t, kClasses> MarkerFns(std::index_sequence<I...>) noexcept
{
    return {reinterpret_cast<std::uintptr_t>(&Markers<I>)...};
}

std::wstring Report()
{
    std::wstring out = g_installed ? L"installed" : L"not installed";
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const SeatState& state = g_seats[seat];
        int              in    = 0;
        for (const Shadow& shadow : state.shadows)
        {
            in += shadow.actor != nullptr && UserOf(shadow).Set() ? 1 : 0;
        }
        out += std::format(L"; {}: at {} spot(s), enters {}, leaves {}, held back {}, starts {}", players::SeatName(seat), in,
                           state.enters, state.leaves, state.heldBack, state.requests);
    }
    return out;
}

/// The most entries of the manager's spot map read in one search (a field lists a few hundred).
constexpr std::int32_t kMaxSpotEntries = 4096;

}  // namespace

bool ForEachSpot(const std::function<void(void* actor, std::uint64_t tag)>& fn) noexcept
try
{
    auto* manager = g_installed ? static_cast<std::uint8_t*>(g_fieldActionMgr()) : nullptr;
    platform::GameArray pairs;
    if (manager == nullptr || !Read(manager + facts::kFieldActionMgrVolumes, pairs) || pairs.data == nullptr)
    {
        return false;
    }
    for (std::int32_t i = 0; i < pairs.num && i < kMaxSpotEntries; ++i)
    {
        const auto*         pair  = static_cast<const std::uint8_t*>(pairs.data) + static_cast<std::size_t>(i) * facts::kNameMapPairSize;
        std::uint64_t       key   = 0;
        void*               actor = nullptr;
        platform::GameArray tags;
        std::uint64_t       first = 0;
        if (Read(pair, key) && Read(pair + facts::kNameMapPairValue, actor) && actor != nullptr &&
            Read(static_cast<const std::uint8_t*>(actor) + facts::kActorTags, tags) && tags.num >= 1 && Read(tags.data, first) &&
            first == key)
        {
            fn(actor, key);
        }
    }
    return true;
}
catch (...)
{
    return false;
}

bool AiOnlySpotNear(const platform::GameVector& at, float radius, AiOnlySpot& out) noexcept
try
{
    bool found = false;
    ForEachSpot([&](void* actor, std::uint64_t key) {
        const std::size_t cls  = ClassOfVtable(actor);
        const auto*       spot = static_cast<const std::uint8_t*>(actor);
        // NPC-only by its byte, with its collision off (the state's effect, so a byte left set by another state counts
        // for nothing), and not deactivated.
        std::uint8_t npcOnly = 0, activation = 0, collision = 0;
        if (cls == kClasses || !Read(spot + facts::kFieldSpotNpcOnly, npcOnly) || npcOnly == 0 ||
            !Read(spot + facts::kActorCollisionByte, collision) || (collision & facts::kActorCollisionBit) != 0 ||
            !Read(spot + facts::kFieldSpotActivation, activation) || activation == facts::kFieldSpotDeactivated)
        {
            return;
        }
        platform::GameVector where;
        if (!actor_place::ActorLocation(actor, where))
        {
            return;
        }
        const float distance = std::hypot(where.x - at.x, where.y - at.y);
        if (distance <= radius && (!found || distance < out.distance))
        {
            out   = AiOnlySpot{actor, facts::kWalkUpClasses[cls].name, platform::NameText(key), distance};
            found = true;
        }
    });
    return found;
}
catch (...)
{
    return false;
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto fail = [](const wchar_t* what) {
        platform::LogWarning(L"walk-up: {} is not as the research read it; further players cannot use ladders, squeezes and "
                             L"other walk-up spots", what);
        return false;
    };
    if (!players::weak_object::Installed())
    {
        return fail(L"the weak-pointer helper");
    }
    const auto sweepSlot = image.Rebase(facts::kPlayerTriggerSweepSlot, 8);
    const auto sweep     = platform::FindFunction(image, facts::kPlayerTriggerSweep);
    const auto pmGet     = platform::FindFunction(image, facts::kPartyManagerGet);
    const auto mgrGet    = platform::FindFunction(image, facts::kFieldActionMgrGet);
    const auto find      = platform::FindFunction(image, facts::kNameMapFind);
    const auto begin     = platform::FindFunction(image, facts::kTriggerBeginRouter);
    const auto end       = platform::FindFunction(image, facts::kTriggerEndRouter);
    const auto naviEnter = platform::FindFunction(image, facts::kNaviEnterArea);
    const auto naviLeave = platform::FindFunction(image, facts::kNaviLeaveArea);
    const auto naviRem   = platform::FindFunction(image, facts::kNaviListRemove);
    const auto pending   = image.Rebase(facts::kPendingOverlapNames, 16);
    const auto name      = image.Rebase(facts::kFNamePlayerTrigger, 8);
    const auto party     = players::game_globals::Address(players::game_globals::Global::PartyManager);
    if (!sweepSlot || !sweep || !pmGet || !mgrGet || !find || !begin || !end || !naviEnter || !naviLeave || !naviRem || !pending ||
        !name || !players::field_root::Installed() || !party)
    {
        return fail(L"a function or global");
    }
    std::array<std::uintptr_t, kClasses> slots{}, markerSlots{}, tryStartFns{}, markerFns{};
    for (std::size_t cls = 0; cls < kClasses; ++cls)
    {
        const auto& c      = facts::kWalkUpClasses[cls];
        const auto  vtable = image.Rebase(c.vtable, facts::kVtLeave + 8);
        const auto  tries  = image.Rebase(c.tryStart), enters = image.Rebase(c.enter), leaves = image.Rebase(c.leave);
        const auto  marks  = image.Rebase(c.markers);
        std::uintptr_t live[4]{};
        if (!vtable || !tries || !enters || !leaves || !marks ||
            !platform::Read(reinterpret_cast<const void*>(*vtable + facts::kVtTryStart), live[0]) ||
            !platform::Read(reinterpret_cast<const void*>(*vtable + facts::kVtMarkers), live[1]) ||
            !platform::Read(reinterpret_cast<const void*>(*vtable + facts::kVtEnter), live[2]) ||
            !platform::Read(reinterpret_cast<const void*>(*vtable + facts::kVtLeave), live[3]) || live[0] != *tries ||
            live[1] != *marks || live[2] != *enters || live[3] != *leaves)
        {
            return fail(c.name);
        }
        g_vtables[cls]   = *vtable;
        // The pole slide only with its finish's fix (else its end leaves player 1's pole camera).
        g_admitted[cls] = cls != facts::kWalkUpPole || traversal_action::PoleReady();
        if (!g_admitted[cls])
        {
            platform::LogWarning(L"walk-up: the {} is held back for further players (its fix is not installed)", c.name);
        }
        slots[cls]       = *vtable + facts::kVtTryStart;
        markerSlots[cls] = *vtable + facts::kVtMarkers;
        tryStartFns[cls] = *tries;
        markerFns[cls]   = *marks;
    }
    std::uint64_t playerTrigger = 0;
    if (!platform::Read(reinterpret_cast<const void*>(*name), playerTrigger) || playerTrigger == 0)
    {
        return fail(L"the PlayerTrigger name");
    }
    g_partyManagerGet = reinterpret_cast<CellFn>(*pmGet);
    g_fieldActionMgr  = reinterpret_cast<MgrGetFn>(*mgrGet);
    g_nameFind        = reinterpret_cast<NameFindFn>(*find);
    g_pendingNames    = *pending;
    g_playerTrigger   = playerTrigger;
    g_partyManager    = party;
    g_sweepOriginal   = reinterpret_cast<SweepFn>(*sweep);

    const auto call = [&](platform::CallRedirect& redirect, std::uintptr_t site, std::uintptr_t target, auto* replacement) {
        const auto live = image.Rebase(site, 5);
        return live && redirect.Install(*live, target, reinterpret_cast<std::uintptr_t>(replacement));
    };
    const bool placed = call(g_resolve, facts::kDispatchResolveCall, *pmGet, &Resolve) &&
                        call(g_begin, facts::kDispatchBeginRouterCall, *begin, &Begin) &&
                        call(g_end, facts::kDispatchEndRouterCall, *end, &End) &&
                        call(g_naviList, facts::kDispatchNaviListCall, *pmGet, &NaviList) &&
                        call(g_naviEnter, facts::kDispatchNaviEnterCall, *naviEnter, &NaviEnter) &&
                        call(g_naviRemove, facts::kDispatchNaviRemoveCall, *naviRem, &NaviRemove) &&
                        call(g_naviLeave, facts::kDispatchNaviLeaveCall, *naviLeave, &NaviLeave);
    const auto     tryStarts = TryStarts(std::make_index_sequence<kClasses>{});
    const auto     markers   = MarkerFns(std::make_index_sequence<kClasses>{});
    bool           slotsOk   = placed;
    for (std::size_t cls = 0; slotsOk && cls < kClasses; ++cls)
    {
        slotsOk = g_tryStart[cls].Replace(slots[cls], tryStartFns[cls], tryStarts[cls]) &&
                  g_markers[cls].Replace(markerSlots[cls], markerFns[cls], markers[cls]);
    }
    // The sweep last: once it runs, seats' entries reach the dispatcher, whose sites are all in place by then.
    if (!slotsOk || !g_sweep.Replace(*sweepSlot, *sweep, reinterpret_cast<std::uintptr_t>(&Sweep)))
    {
        Remove();
        return fail(L"a call site or vtable slot");
    }
    g_installed = true;
    control::Register({.name  = L"traversal.walkup",
                       .shape = control::Shape::Text,
                       .help  = L"the walk-up spots each further player is at, and their enters, leaves, hold-backs and starts",
                       .get   = [] { return control::Value{Report()}; }});
    platform::Log(L"walk-up: installed; further players' triggers reach the walk-up spots (ladders, squeezes, vaults, slides, beams and more), "
                  L"and the arrows show near any player");
    return true;
}

void TickSeat(int seat, void* controller, void* member) noexcept
{
    if (!g_installed || !players::SeatBuilt(seat))
    {
        return;
    }
    SeatState& state   = g_seats[seat];
    void*      trigger = controller != nullptr ? platform::ReadPointer(controller, facts::kControllerTriggerComponent) : nullptr;
    if (controller != state.controller || trigger != state.trigger)
    {
        for (Shadow& shadow : state.shadows)
        {
            shadow = Shadow{};  // a new trigger (or none): its entered set starts empty, no leave comes for the old one
        }
        if (state.controller != nullptr || controller != nullptr)
        {
            platform::LogDebug(L"walk-up: {}'s spots forgotten ({})", players::SeatName(seat), controller != nullptr ? L"a new controller" : L"the player left");
        }
    }
    if (controller != state.controller && controller != nullptr && trigger == nullptr)
    {
        platform::LogDebug(L"walk-up: {}'s controller has no trigger component yet; its spots start once it has", players::SeatName(seat));
    }
    state.controller = controller;
    state.trigger    = trigger;
    state.member     = member;
}

void Remove() noexcept
{
    g_sweep.Restore();
    for (std::size_t cls = 0; cls < kClasses; ++cls)
    {
        g_markers[cls].Restore();
        g_tryStart[cls].Restore();
    }
    g_naviLeave.Restore();
    g_naviRemove.Restore();
    g_naviEnter.Restore();
    g_naviList.Restore();
    g_end.Restore();
    g_begin.Restore();
    g_resolve.Restore();
    g_installed = false;
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

}  // namespace lss::world::traversal_walkup
