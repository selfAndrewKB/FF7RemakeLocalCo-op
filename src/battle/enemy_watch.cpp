#include "battle/enemy_watch.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/entry_redirect.h"
#include "platform/game_vectors.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/steady_ms.h"
#include "platform/verified_code.h"
#include "players/actor_fields.h"
#include "players/scopes.h"
#include "players/weak_object.h"
#include "world/actor_place.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <format>
#include <intrin.h>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>

namespace lss::battle::enemy_watch {

namespace {

using platform::Read;
using players::weak_object::Weak;
using PawnCompFn    = void* (*)(void* controller);
using IsAFn         = bool (*)(void* object, void* b, void* c);
using CharacterOfFn = void* (*)(const void* targetObject);
using ComponentTest = bool (*)(void* component);           // BattlePlayerComp_IsPartySide, BattleComp_IsHateEnemy
using OwnerFn       = void* (*)(void* component);           // the battle component's owner character (vtable +0x20)
using SetTargetFn   = void (*)(void* component, void* character);

constexpr std::size_t  kEntries    = 32;       // enemies watched at once (a battle has a handful)
constexpr std::int64_t kSeenMs     = 1000;     // a gate not run for this long: the enemy left the battle, or the game paused
constexpr std::int64_t kReclaimMs  = 5000;     // a gate may take over an entry not written for this long
constexpr std::int64_t kFirstWait  = 5000;     // between actions this long: logged
constexpr std::int64_t kNextWait   = 10000;    // and again every this long
constexpr int          kAreaUnset  = INT_MIN;  // no area read since the enemies were last seen
constexpr std::int64_t kSetSeenMs  = 250;      // a polled target change this soon after a target call: that call made it
constexpr unsigned     kFrames     = 3;        // the callers named for a target call

// Written by the gates on any thread; an entry is claimed by a compare-and-swap on its controller from the value the
// claimant saw (empty, or an entry not written for kReclaimMs). The game thread only reads. Two gates racing for one
// stale entry, or a gate writing just as another reclaims its entry, can mix one enemy's fields into another's for a
// tick: a logging glitch, never a read of a destroyed enemy (the game thread reads no pointer from here; see Reported).
struct Entry
{
    std::atomic<std::uintptr_t> controller{0};
    std::atomic<std::uint64_t>  action{0};
    std::atomic<std::uintptr_t> target{0};  // the component's own target object: identity only, never dereferenced here
    std::atomic<std::int64_t>   seenMs{0};
    std::atomic<std::uintptr_t> component{0};     // its battle component: the key the two hooks find the entry by
    std::atomic<int>            ready{0};         // the attack-ready test's last answer (0, 1 light, 2 heavy)
    std::atomic<std::int64_t>   readySinceMs{0};  // when it last turned ready (written before `ready`)
    std::atomic<std::int64_t>   setMs{0};         // the last target call into its component
    // The last target call logged at Debug (its caller's return, the character asked, the one held after), and the
    // exact repeats of it since (logged at Trace; the next differing call's Debug line counts them).
    std::atomic<std::uintptr_t> setCaller{0};
    std::atomic<std::uintptr_t> setAsked{0};
    std::atomic<std::uintptr_t> setHeld{0};
    std::atomic<int>            setRepeats{0};
};
std::array<Entry, kEntries> g_entries;

// What the game thread last logged for each entry. The controller is held by a weak pointer made at first sight; every
// read of the enemy goes through it.
struct Reported
{
    std::uintptr_t controller = 0;  // identity of the entry's enemy (compared, never dereferenced)
    Weak           weak;
    std::uint64_t  action     = 0;
    std::uintptr_t target     = 0;  // the target object last seen
    void*          character  = nullptr;  // the character it named when logged (compared only)
    std::int64_t   sinceMs    = 0;  // when the action (or the wait between actions) began
    std::int64_t   nextWaitMs = 0;  // when the next "no action" line is due
    int            ready       = 0;  // the ready answer last logged
    std::int64_t   readyNextMs = 0;  // when the next "ready without acting" line is due
    std::wstring   name;
};
std::array<Reported, kEntries> g_reported;  // game thread
// Per entry, the last write time at which its enemy was found gone (destroyed, or not a live battle AI controller): a
// first sight is taken only from a later write, so a dead enemy's address is never touched again. Game thread.
std::array<std::int64_t, kEntries> g_goneSeenMs{};
std::atomic<bool>                  g_installed{false};
std::atomic<bool>                  g_actionsWanted{false};  // another module reads the running actions (RunningOneOf)
PawnCompFn                        g_pawnComp = nullptr;  // set at install (published by g_installed), then only read
IsAFn                              g_isAi     = nullptr;
ComponentTest                      g_isPartySide = nullptr;
ComponentTest                      g_isHate      = nullptr;
std::uintptr_t                     g_gameBase    = 0;  // the game module's live range, for naming callers
std::uintptr_t                     g_gameEnd     = 0;
platform::EntryRedirect            g_setTarget;

// Each human's member (player 1: the game's leader), refreshed by the game thread each tick for the hooks on any
// thread. Compared only, never dereferenced from here.
std::array<std::atomic<void*>, players::scopes::kMaxPlayers> g_members{};

// Each human's member's battle area as last logged (points; 0 empty; -1 unreadable; kAreaUnset none yet), and when it
// was last read. Game thread.
std::array<int, players::scopes::kMaxPlayers> g_areaPoints{};  // filled with kAreaUnset at install
std::int64_t                                  g_areaReadMs = 0;

using platform::NowMs;

std::wstring ActionName(std::uint64_t bits) noexcept
{
    if (bits == 0)
    {
        return L"no action";
    }
    std::wstring text = platform::NameText(bits);
    if (text.empty())
    {
        return std::format(L"name#{:x}", bits);
    }
    return text;
}

void ClassText(void* object, std::wstring* out) noexcept
try
{
    *out = static_cast<RC::Unreal::UObject*>(object)->GetClassPrivate()->GetName();
}
catch (...)
{
    out->clear();
}

/// A live object's class name. Game thread.
std::wstring ClassName(void* object) noexcept
{
    std::wstring text;
    if (object == nullptr || !platform::CallGuarded(&ClassText, object, &text) || text.empty())
    {
        return L"(unnamed)";
    }
    return text;
}

/// Player `seat + 1`'s member: the game's leader for player 1 (as battle/enemy_coin's Tick reads it), the bound member
/// for the others; null if none. Game thread (Tick copies it into g_members for the hooks).
void* ReadMember(int seat) noexcept
{
    if (seat > 0)
    {
        return players::scopes::Member(seat);
    }
    const std::uintptr_t manager = players::scopes::PartyManagerAddress();
    std::int32_t         leader[2]{};
    return manager != 0 && Read(reinterpret_cast<const void*>(manager + facts::kPartyLeaderWeak), leader)
               ? players::weak_object::ObjectOf(leader)
               : nullptr;
}

/// Player `seat + 1`'s member as the game thread last copied it. Any thread.
void* MemberOf(int seat) noexcept
{
    return g_members[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed);
}

/// A battle component's owner character, through the component's own getter. Null if unreadable.
void* OwnerOf(void* component) noexcept
{
    void**  vtable = nullptr;
    OwnerFn get    = nullptr;
    void*   owner  = nullptr;
    return component != nullptr && Read(component, vtable) && Read(vtable + facts::kComponentOwnerGetterSlot, get) &&
                   platform::CallGuarded(get, owner, component)
               ? owner
               : nullptr;
}

/// The 3D distance between two characters in metres, as text.
std::wstring Metres(const void* from, const void* to) noexcept
{
    platform::GameVector a, b;
    if (!world::actor_place::ActorLocation(from, a) || !world::actor_place::ActorLocation(to, b))
    {
        return L"an unread distance";
    }
    return std::format(L"{:.1f} m", platform::Length(b - a) / 100.0f);
}

/// A caller of the target call, as IDA names it: a known setter by name, else the IDA address of the return.
std::wstring FrameText(void* frame) noexcept
{
    const auto live = reinterpret_cast<std::uintptr_t>(frame);
    if (live < g_gameBase || live >= g_gameEnd)
    {
        return L"code outside the game";
    }
    const std::uintptr_t ida   = platform::IdaAddress(live);
    DWORD64              base  = 0;
    const auto*          entry = RtlLookupFunctionEntry(live, &base, nullptr);
    if (entry != nullptr)
    {
        const std::uintptr_t start = platform::IdaAddress(static_cast<std::uintptr_t>(base) + entry->BeginAddress);
        for (const auto& setter : facts::kTargetSetters)
        {
            if (setter.start == start)
            {
                return std::format(L"{} ({:#x})", setter.name, ida);
            }
        }
    }
    return std::format(L"{:#x}", ida);
}

/// The live controller of a reported enemy, or null once the engine destroyed it (the weak pointer resolves to that
/// same address, never to an object that took the index over). Game thread.
void* ControllerOf(const Reported& r) noexcept
{
    void* const live = r.controller != 0 ? players::weak_object::Resolve(r.weak) : nullptr;
    return live == reinterpret_cast<void*>(r.controller) ? live : nullptr;
}

/// Whether a battle AI controller lives at `key`: checked without a write (weak_object::Live) before the class test,
/// so a freed address is never dereferenced beyond guarded reads. Game thread.
bool LiveController(std::uintptr_t key) noexcept
{
    void* const address = reinterpret_cast<void*>(key);
    bool        isAi    = false;
    return players::weak_object::Live(address) && platform::CallGuarded(g_isAi, isAi, address, nullptr, nullptr) && isAi;
}

/// The live controller's battle component, through the game's getter; null if none.
void* ComponentOf(void* controller) noexcept
{
    void* component = nullptr;
    return controller != nullptr && platform::CallGuarded(g_pawnComp, component, controller) ? component : nullptr;
}

/// The character a battle component targets: the component's own target (the object at +0x38, live while its
/// reference block's strong count is above 0; the object's vtable +0x20 names the character, as
/// BattlePlayerComp_GetCurrentTarget's callers read it). The game's getter itself is not called: it takes a reference
/// on the pair, which only the game's own release may drop. Null if none. The game thread between AI ticks, or the
/// thread writing the pair (the target-call hook, around the game's own write).
void* TargetCharacterOf(const void* component) noexcept
{
    if (component == nullptr)
    {
        return nullptr;
    }
    // The block, object, block read only adds a check for a writer that stores the object before the block; one
    // storing the block first would pass it.
    const auto*  base   = static_cast<const std::uint8_t*>(component);
    void*        object = nullptr;
    const auto*  block  = static_cast<const std::uint8_t*>(platform::ReadPointer(base, facts::kBattleCompOwnTargetRef));
    std::int32_t strong = 0;
    if (block == nullptr || !Read(base + facts::kBattleCompOwnTarget, object) || object == nullptr ||
        platform::ReadPointer(base, facts::kBattleCompOwnTargetRef) != block || !Read(block + facts::kSharedRefStrongCount, strong) ||
        strong <= 0)
    {
        return nullptr;
    }
    void**        vtable = nullptr;
    CharacterOfFn of     = nullptr;
    void*         target = nullptr;
    return Read(object, vtable) && Read(vtable + facts::kTargetObjectCharacterSlot, of) && platform::CallGuarded(of, target, object)
               ? target
               : nullptr;
}

/// The character a live enemy controller's pawn targets. Game thread: safe because no AI writer replaces the pair
/// during this pre-tick (the AI tick may run off the game thread, ASSUMPTION: only inside the world tick).
void* TargetOf(void* controller) noexcept
{
    return TargetCharacterOf(ComponentOf(controller));
}

/// The enemy character a live controller drives (its battle component's owner).
void* EnemyOf(void* controller) noexcept
{
    return OwnerOf(ComponentOf(controller));
}

/// A character as a player would name it: whose member it is, else its class. Game thread.
std::wstring CharacterName(void* character) noexcept
{
    if (character == nullptr)
    {
        return L"nobody";
    }
    for (int seat = 0; seat < players::scopes::kMaxPlayers; ++seat)
    {
        if (MemberOf(seat) == character)
        {
            return std::format(L"player {}'s member ({})", seat + 1, ClassName(character));
        }
    }
    return ClassName(character);
}

/// How far `enemy` is from `target` and from each other human's member: "<target> at 8.2 m; player 2's member at
/// 23.1 m".
std::wstring Distances(const void* enemy, void* target) noexcept
{
    std::wstring text = target != nullptr ? std::format(L"{} at {}", CharacterName(target), Metres(enemy, target)) : L"no target";
    for (int seat = 0; seat < players::scopes::kMaxPlayers; ++seat)
    {
        if (void* member = MemberOf(seat); member != nullptr && member != target)
        {
            text += std::format(L"; player {}'s member at {}", seat + 1, Metres(enemy, member));
        }
    }
    return text;
}

/// The entry holding `component`, or null.
Entry* EntryOf(const void* component) noexcept
{
    const auto key = reinterpret_cast<std::uintptr_t>(component);
    for (auto& e : g_entries)
    {
        if (e.component.load(std::memory_order_relaxed) == key)
        {
            return &e;
        }
    }
    return nullptr;
}

// --- The two hooks (any thread) ----------------------------------------------------------------------------------

/// The Debug line of a target call that differs from the enemy's last one (its frames captured here, off the
/// repeat path). Allocates: the caller catches. Never inlined: its frame count skips itself and SetTarget.
__declspec(noinline) void LogTargetCall(void* component, void* character, void* before, void* after, int repeats)
{
    void*          frames[kFrames]{};
    const unsigned count = RtlCaptureStackBackTrace(2, kFrames, frames, nullptr);  // from SetTarget's caller on
    std::wstring   by    = count > 0 ? FrameText(frames[0]) : std::wstring(L"an unread caller");
    for (unsigned i = 1; i < count; ++i)
    {
        by += L", from " + FrameText(frames[i]);
    }
    bool hate = false;
    platform::CallGuarded(g_isHate, hate, component);
    void* const enemy      = OwnerOf(component);
    void* const controller = enemy != nullptr ? players::actor_fields::BattleAi(enemy) : nullptr;
    platform::LogDebug(L"enemy watch: {} {:#x} target call by {}: asked {}, holds {}{}, held {}; {}{}", ClassName(controller),
                       reinterpret_cast<std::uintptr_t>(controller), by, CharacterName(character), CharacterName(after),
                       hate ? L" (a hate enemy: the call takes its hate pick)" : L"", CharacterName(before), Distances(enemy, after),
                       repeats > 0 ? std::format(L" (the call before it repeated {} times)", repeats) : std::wstring());
}

/// BattleComp_SetTargetChar: the game's call unchanged; for an enemy's component, who called it and what it holds.
/// A call repeating the enemy's last one exactly (same caller, asked and held character; a script asking each tick)
/// is a Trace line; any other is a Debug line.
void SetTarget(void* component, void* character)
{
    const auto original = reinterpret_cast<SetTargetFn>(g_setTarget.Original());
    bool       party    = true;
    if (!Watching() || !platform::CallGuarded(g_isPartySide, party, component) || party)
    {
        original(component, character);
        return;
    }
    const auto  caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    void* const before = TargetCharacterOf(component);
    original(component, character);
    try
    {
        void* const    after  = TargetCharacterOf(component);
        const auto     asked  = reinterpret_cast<std::uintptr_t>(character);
        const auto     held   = reinterpret_cast<std::uintptr_t>(after);
        Entry* const   entry  = EntryOf(component);
        int            repeats = 0;
        if (entry != nullptr)
        {
            entry->setMs.store(NowMs(), std::memory_order_relaxed);
            if (entry->setCaller.load(std::memory_order_relaxed) == caller && entry->setAsked.load(std::memory_order_relaxed) == asked &&
                entry->setHeld.load(std::memory_order_relaxed) == held)
            {
                entry->setRepeats.fetch_add(1, std::memory_order_relaxed);
                platform::LogTrace(L"enemy watch: target call {:#x} repeated (caller {:#x})", reinterpret_cast<std::uintptr_t>(component),
                                   platform::IdaAddress(caller));
                return;
            }
            entry->setCaller.store(caller, std::memory_order_relaxed);
            entry->setAsked.store(asked, std::memory_order_relaxed);
            entry->setHeld.store(held, std::memory_order_relaxed);
            repeats = entry->setRepeats.exchange(0, std::memory_order_relaxed);
        }
        LogTargetCall(component, character, before, after, repeats);
    }
    catch (...)
    {
        // A failed log line (an allocation) never unwinds into the game's frames.
    }
}


/// The points of `member`'s first area query (the area enemies read around the character they target); -1 when
/// unreadable or the member has no query.
int AreaPoints(const void* member) noexcept
{
    const auto*  queries = static_cast<const std::uint8_t*>(platform::ReadPointer(member, facts::kCharacterAreaQueries));
    std::int32_t count   = 0;
    if (queries == nullptr || !Read(queries + facts::kAreaQueryCount, count) || count <= 0)
    {
        return -1;
    }
    const auto*  first  = static_cast<const std::uint8_t*>(platform::ReadPointer(queries, facts::kAreaQueryEntries));
    std::int32_t points = 0;
    return first != nullptr && Read(first + facts::kAreaEntryPointCount, points) ? points : -1;
}

std::wstring AreaText(int points) noexcept
{
    return points > 0 ? std::format(L"has {} points", points) : std::wstring(points == 0 ? L"is empty" : L"is unreadable");
}

/// The area's state for the change test: points, empty, unreadable.
int AreaState(int points) noexcept
{
    return points > 0 ? 1 : points;
}

/// Logs each human's member's area at a change between having points, empty and unreadable (once a second while
/// enemies are seen). Game thread.
void ReadAreas(std::int64_t now) noexcept
{
    if (now - g_areaReadMs < 1000)
    {
        return;
    }
    g_areaReadMs = now;
    for (int seat = 0; seat < players::scopes::kMaxPlayers; ++seat)
    {
        void* member = MemberOf(seat);
        int&  last   = g_areaPoints[static_cast<std::size_t>(seat)];
        if (member == nullptr)
        {
            last = kAreaUnset;
            continue;
        }
        const int points = AreaPoints(member);
        if (last == kAreaUnset || AreaState(points) != AreaState(last))
        {
            platform::LogDebug(L"enemy watch: player {}'s member ({}): its battle area {}", seat + 1, ClassName(member), AreaText(points));
        }
        last = points;
    }
}

double Seconds(std::int64_t ms) noexcept
{
    return static_cast<double>(ms) / 1000.0;
}

/// The `enemy-watch` reading. Game thread.
std::wstring Reading() noexcept
{
    if (!Watching())
    {
        return L"the watch runs while the log writes Debug lines (log-level debug)";
    }
    const auto   now = NowMs();
    std::wstring text;
    for (std::size_t i = 0; i < kEntries; ++i)
    {
        const Reported& r = g_reported[i];
        void* const     controller = ControllerOf(r);
        if (controller == nullptr || now - g_entries[i].seenMs.load(std::memory_order_acquire) > kSeenMs)
        {
            continue;
        }
        const int ready = g_entries[i].ready.load(std::memory_order_acquire);
        text += std::format(L"{}{} {:#x}: {} for {:.1f} s on {}{}", text.empty() ? L"" : L"; ", r.name, r.controller, ActionName(r.action),
                            Seconds(now - r.sinceMs), Distances(EnemyOf(controller), TargetOf(controller)),
                            ready == 0 ? L"" : ready == 2 ? L", ready (heavy)" : L", ready (light)");
    }
    if (text.empty())
    {
        text = L"no enemy seen";
    }
    for (int seat = 0; seat < players::scopes::kMaxPlayers; ++seat)
    {
        if (void* member = MemberOf(seat); member != nullptr)
        {
            text += std::format(L"; player {}'s member's battle area {}", seat + 1, AreaText(AreaPoints(member)));
        }
    }
    return text;
}

}  // namespace

bool Watching() noexcept
{
    return g_installed.load(std::memory_order_acquire) && platform::Enabled(platform::Level::Debug);
}

bool Recording() noexcept
{
    return g_installed.load(std::memory_order_acquire) &&
           (g_actionsWanted.load(std::memory_order_relaxed) || platform::Enabled(platform::Level::Debug));
}

void WantActions(bool on) noexcept
{
    g_actionsWanted.store(on, std::memory_order_relaxed);
}

std::uint64_t RunningOneOf(std::span<const std::uint64_t> actions) noexcept
{
    if (actions.empty() || !Recording())
    {
        return 0;
    }
    const auto now = NowMs();
    for (const Entry& e : g_entries)
    {
        if (e.controller.load(std::memory_order_relaxed) == 0 || now - e.seenMs.load(std::memory_order_acquire) > kSeenMs)
        {
            continue;
        }
        const std::uint64_t action = e.action.load(std::memory_order_relaxed);
        if (action != 0 && std::find(actions.begin(), actions.end(), action) != actions.end())
        {
            return action;
        }
    }
    return 0;
}

void Ready(const void* component, char ready) noexcept
{
    if (!Watching())
    {
        return;
    }
    if (Entry* entry = EntryOf(component); entry != nullptr)
    {
        if (ready != 0 && entry->ready.load(std::memory_order_relaxed) == 0)
        {
            entry->readySinceMs.store(NowMs(), std::memory_order_relaxed);
        }
        entry->ready.store(ready, std::memory_order_release);
    }
}

void* TargetCharacter(const void* component) noexcept
{
    return TargetCharacterOf(component);
}

std::wstring TargetAndDistances(void* component) noexcept
try
{
    return Distances(OwnerOf(component), TargetCharacterOf(component));
}
catch (...)
{
    return L"an unread target";
}

void Observe(void* controller, const void* component, std::uint64_t action) noexcept
{
    const auto     key    = reinterpret_cast<std::uintptr_t>(controller);
    const auto     now    = NowMs();
    std::uintptr_t target = 0;
    Read(static_cast<const std::uint8_t*>(component) + facts::kBattleCompOwnTarget, target);

    Entry* entry = nullptr;
    for (auto& e : g_entries)
    {
        if (e.controller.load(std::memory_order_relaxed) == key)
        {
            entry = &e;
            break;
        }
    }
    for (std::size_t i = 0; entry == nullptr && i < kEntries; ++i)
    {
        Entry&         e    = g_entries[i];
        std::uintptr_t seen = e.controller.load(std::memory_order_relaxed);
        if (seen != 0 && now - e.seenMs.load(std::memory_order_relaxed) <= kReclaimMs)
        {
            continue;  // another enemy's, written lately
        }
        if (e.controller.compare_exchange_strong(seen, key, std::memory_order_relaxed))
        {
            entry = &e;  // claimed: the previous enemy's hook fields start over, its component key first
            e.component.store(0, std::memory_order_relaxed);
            e.action.store(0, std::memory_order_relaxed);
            e.ready.store(0, std::memory_order_relaxed);
            e.setMs.store(0, std::memory_order_relaxed);
            e.setCaller.store(0, std::memory_order_relaxed);
            e.setRepeats.store(0, std::memory_order_relaxed);
        }
        else if (seen == key)
        {
            entry = &e;  // the same enemy claimed it on another thread meanwhile
        }
    }
    if (entry == nullptr)
    {
        return;  // more enemies than entries: the extra ones go unwatched
    }
    entry->component.store(reinterpret_cast<std::uintptr_t>(component), std::memory_order_relaxed);
    entry->action.store(action, std::memory_order_relaxed);
    entry->target.store(target, std::memory_order_relaxed);
    entry->seenMs.store(now, std::memory_order_release);
}

bool Install(const platform::GameImage& image) noexcept
{
    if (!players::weak_object::Installed())
    {
        platform::LogWarning(L"enemy watch: no game object can be kept between ticks (players/weak_object); the watch is off");
        return false;
    }
    const auto pawnComp = platform::FindFunction(image, facts::kBattleAIGetPawnComp);
    const auto isAi     = platform::FindFunction(image, facts::kIsABattleAIController);
    if (!pawnComp || !isAi)
    {
        platform::LogWarning(L"enemy watch: the controller's component getter or class test differs from the research; the watch is off");
        return false;
    }
    g_pawnComp = reinterpret_cast<PawnCompFn>(*pawnComp);
    g_isAi     = reinterpret_cast<IsAFn>(*isAi);
    g_areaPoints.fill(kAreaUnset);

    // Who aims an enemy, and when it is ready: two entry redirects, each optional (the watch runs without them).
    MODULEINFO module{};
    if (K32GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(platform::kGameExe), &module, sizeof(module)) != 0)
    {
        g_gameBase = reinterpret_cast<std::uintptr_t>(module.lpBaseOfDll);
        g_gameEnd  = g_gameBase + module.SizeOfImage;
    }
    else
    {
        platform::LogWarning(L"enemy watch: the game module's range is unread; target calls name no caller");
    }
    const auto setTarget   = platform::FindFunction(image, facts::kBattleCompSetTargetChar);
    const auto isPartySide = platform::FindFunction(image, facts::kBattleCompIsPartySide);
    const auto isHate      = platform::FindFunction(image, facts::kBattleCompIsHateEnemy);
    if (isPartySide && isHate)
    {
        g_isPartySide = reinterpret_cast<ComponentTest>(*isPartySide);
        g_isHate      = reinterpret_cast<ComponentTest>(*isHate);
    }
    if (!g_isPartySide || !setTarget ||
        !g_setTarget.Install(*setTarget, facts::kBattleCompSetTargetCharFirst, reinterpret_cast<std::uintptr_t>(&SetTarget)))
    {
        platform::LogWarning(L"enemy watch: the target call could not be redirected; who aims an enemy is not logged");
    }
    control::Register({.name  = L"enemy-watch",
                       .shape = control::Shape::Text,
                       .help  = L"each enemy's running action and target, and each human's member's battle area, while the log writes "
                                L"Debug lines (battle/enemy_watch.h)",
                       .get   = [] { return control::Value{Reading()}; }});
    g_installed.store(true, std::memory_order_release);
    return true;
}

void Remove() noexcept
{
    g_setTarget.Restore();
}

void Tick() noexcept
{
    if (!Watching())
    {
        return;
    }
    for (int seat = 0; seat < players::scopes::kMaxPlayers; ++seat)
    {
        g_members[static_cast<std::size_t>(seat)].store(ReadMember(seat), std::memory_order_relaxed);
    }
    const auto now    = NowMs();
    bool       anyone = false;
    for (std::size_t i = 0; i < kEntries; ++i)
    {
        const Entry&         e    = g_entries[i];
        Reported&            r    = g_reported[i];
        const std::uintptr_t key  = e.controller.load(std::memory_order_relaxed);
        const std::int64_t   seen = e.seenMs.load(std::memory_order_acquire);
        if (key == 0 || now - seen > kSeenMs)
        {
            if (r.controller != 0)
            {
                platform::LogDebug(L"enemy watch: {} {:#x} not seen for a second: left the battle, or the game paused ({} for {:.1f} s)",
                                   r.name, r.controller, ActionName(r.action), Seconds(now - r.sinceMs));
                r = Reported{};
            }
            continue;
        }
        const std::uint64_t  action = e.action.load(std::memory_order_relaxed);
        const std::uintptr_t target = e.target.load(std::memory_order_relaxed);
        if (r.controller != key)
        {
            // First sight, only from a write after the entry's enemy was last found gone, and only of a live battle
            // AI controller (a gate written last frame may name one a garbage collection has freed since); from here
            // on it is read only through the weak pointer.
            if (seen <= g_goneSeenMs[i])
            {
                continue;
            }
            if (!LiveController(key))
            {
                platform::LogTrace(L"enemy watch: {:#x} is not a live battle AI controller; not watched", key);
                g_goneSeenMs[i] = seen;
                continue;
            }
            r            = Reported{};
            r.controller = key;
            r.weak       = players::weak_object::MakeWeak(reinterpret_cast<void*>(key));
            void* live   = ControllerOf(r);
            if (live == nullptr)
            {
                platform::LogDebug(L"enemy watch: no weak pointer could be made for the live controller {:#x}; not watched", key);
                g_goneSeenMs[i] = seen;
                r               = Reported{};
                continue;
            }
            anyone      = true;
            r.name      = ClassName(live);
            r.action    = action;
            r.target    = target;
            r.character = TargetOf(live);
            r.sinceMs   = now;
            r.nextWaitMs = now + kFirstWait;
            platform::LogDebug(L"enemy watch: {} {:#x} is in the battle: {} on {}", r.name, key, ActionName(action),
                               Distances(EnemyOf(live), r.character));
            continue;
        }
        void* const live = ControllerOf(r);
        if (live == nullptr)
        {
            platform::LogDebug(L"enemy watch: {} {:#x} is gone (destroyed) ({} for {:.1f} s)", r.name, r.controller, ActionName(r.action),
                               Seconds(now - r.sinceMs));
            g_goneSeenMs[i] = seen;  // a new enemy at the same address is taken at its first later write
            r               = Reported{};
            continue;
        }
        anyone            = true;
        void* const enemy = EnemyOf(live);
        // The ready answer as the hook last wrote it (its time written before it); an action start reads it first, so
        // the start line can say how long the enemy was ready.
        const int          ready      = e.ready.load(std::memory_order_acquire);
        const std::int64_t readySince = e.readySinceMs.load(std::memory_order_relaxed);
        if (action != r.action)
        {
            if (r.action != 0)
            {
                platform::LogDebug(L"enemy watch: {} {:#x} ended {} after {:.1f} s", r.name, key, ActionName(r.action), Seconds(now - r.sinceMs));
            }
            r.target    = target;
            r.character = TargetOf(live);
            if (action != 0)
            {
                const std::wstring after = r.ready != 0 ? std::format(L", ready for {:.1f} s", Seconds(now - readySince)) : std::wstring();
                platform::LogDebug(L"enemy watch: {} {:#x} starts {} on {}{}", r.name, key, ActionName(action), Distances(enemy, r.character),
                                   after);
            }
            r.action     = action;
            r.sinceMs    = now;
            r.nextWaitMs = now + kFirstWait;
        }
        else if (target != r.target)
        {
            r.target              = target;
            void* const character = TargetOf(live);
            if (character != r.character)  // a new target object can name the same character
            {
                r.character = character;
                const bool called = now - e.setMs.load(std::memory_order_relaxed) <= kSetSeenMs;
                platform::LogDebug(L"enemy watch: {} {:#x} now targets {} ({}){}", r.name, key, Distances(enemy, character),
                                   action != 0 ? ActionName(action) : std::wstring(L"between actions"),
                                   called ? L"" : L"; no target call seen: set by a path the call does not cover");
            }
        }
        if (ready != r.ready)
        {
            if (ready != 0 && r.ready == 0)
            {
                platform::LogDebug(L"enemy watch: {} {:#x} is ready for a {} attack on {}", r.name, key, ready == 2 ? L"heavy" : L"light",
                                   Distances(enemy, TargetOf(live)));
                r.readyNextMs = now + kFirstWait;
            }
            else if (ready == 2)
            {
                platform::LogDebug(L"enemy watch: {} {:#x} is ready for a heavy attack (its ATB is full) after {:.1f} s ready", r.name, key,
                                   Seconds(now - readySince));
            }
            else if (ready == 0)
            {
                platform::LogDebug(L"enemy watch: {} {:#x} is no longer ready ({})", r.name, key,
                                   action != 0 ? L"it started " + ActionName(action) : std::wstring(L"its decision gate closed"));
            }
            r.ready = ready;
        }
        else if (ready != 0 && action == 0 && now >= r.readyNextMs)
        {
            platform::LogDebug(L"enemy watch: {} {:#x} has been ready for {:.1f} s without acting; {}", r.name, key, Seconds(now - readySince),
                               Distances(enemy, TargetOf(live)));
            r.readyNextMs = now + kNextWait;
        }
        if (action == 0 && now >= r.nextWaitMs)
        {
            platform::LogDebug(L"enemy watch: {} {:#x} has started no action for {:.1f} s; {}", r.name, key, Seconds(now - r.sinceMs),
                               Distances(enemy, TargetOf(live)));
            r.nextWaitMs = now + kNextWait;
        }
    }
    if (anyone)
    {
        ReadAreas(now);  // in battle: enemies are seen
    }
    else
    {
        g_areaPoints.fill(kAreaUnset);  // the next battle logs every member's area once
    }
}

}  // namespace lss::battle::enemy_watch
