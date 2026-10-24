#include "battle/enemy_watch.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/scopes.h"
#include "players/weak_object.h"

#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::battle::enemy_watch {

namespace {

using platform::Read;
using players::weak_object::Weak;
using PawnCompFn    = void* (*)(void* controller);
using IsAFn         = bool (*)(void* object, void* b, void* c);
using CharacterOfFn = void* (*)(const void* targetObject);

constexpr std::size_t  kEntries    = 32;       // enemies watched at once (a battle has a handful)
constexpr std::int64_t kSeenMs     = 1000;     // a gate not run for this long: the enemy left the battle, or the game paused
constexpr std::int64_t kReclaimMs  = 5000;     // a gate may take over an entry not written for this long
constexpr std::int64_t kFirstWait  = 5000;     // between actions this long: logged
constexpr std::int64_t kNextWait   = 10000;    // and again every this long
constexpr int          kAreaUnset  = INT_MIN;  // no area read since the enemies were last seen

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
    std::wstring   name;
};
std::array<Reported, kEntries> g_reported;  // game thread
// Per entry, the last write time at which its enemy was found gone (destroyed, or not a live battle AI controller): a
// first sight is taken only from a later write, so a dead enemy's address is never touched again. Game thread.
std::array<std::int64_t, kEntries> g_goneSeenMs{};
std::atomic<bool>                  g_installed{false};
PawnCompFn                         g_pawnComp = nullptr;  // set at install (published by g_installed), then only read
IsAFn                              g_isAi     = nullptr;

// Each human's member's battle area as last logged (points; 0 empty; -1 unreadable; kAreaUnset none yet), and when it
// was last read. Game thread.
std::array<int, players::scopes::kMaxPlayers> g_areaPoints{};  // filled with kAreaUnset at install
std::int64_t                                  g_areaReadMs = 0;

std::int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void NameText(std::int64_t bits, std::wstring* out) noexcept
try
{
    *out = RC::Unreal::FName(bits).ToString();
}
catch (...)
{
    out->clear();
}

std::wstring ActionName(std::uint64_t bits) noexcept
{
    if (bits == 0)
    {
        return L"no action";
    }
    std::wstring text;
    if (!platform::CallGuarded(&NameText, static_cast<std::int64_t>(bits), &text) || text.empty())
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
/// for the others; null if none. Game thread.
void* MemberOf(int seat) noexcept
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

/// The character a live enemy controller's pawn targets: its battle component through the game's getter, then the
/// component's own target (the object at +0x38, live while its reference block's strong count is above 0; the
/// object's vtable +0x20 names the character, as BattlePlayerComp_GetCurrentTarget's callers read it). The game's
/// getter itself is not called: it takes a reference on the pair, which only the game's own release may drop. Null if
/// none. Game thread.
void* TargetOf(void* controller) noexcept
{
    void* component = nullptr;
    if (controller == nullptr || !platform::CallGuarded(g_pawnComp, component, controller) || component == nullptr)
    {
        return nullptr;
    }
    // Safe because no AI writer replaces the pair during this pre-tick (the AI tick may run off the game thread,
    // ASSUMPTION: only inside the world tick). The block, object, block read only adds a check for a writer that stores
    // the object before the block; one storing the block first would pass it.
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
        text += std::format(L"{}{} {:#x}: {} for {:.1f} s on {}", text.empty() ? L"" : L"; ", r.name, r.controller, ActionName(r.action),
                            Seconds(now - r.sinceMs), CharacterName(TargetOf(controller)));
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
        if (e.controller.compare_exchange_strong(seen, key, std::memory_order_relaxed) || seen == key)
        {
            entry = &e;  // claimed, or the same enemy claimed it on another thread meanwhile
        }
    }
    if (entry == nullptr)
    {
        return;  // more enemies than entries: the extra ones go unwatched
    }
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
    control::Register({.name  = L"enemy-watch",
                       .shape = control::Shape::Text,
                       .help  = L"each enemy's running action and target, and each human's member's battle area, while the log writes "
                                L"Debug lines (battle/enemy_watch.h)",
                       .get   = [] { return control::Value{Reading()}; }});
    g_installed.store(true, std::memory_order_release);
    return true;
}

void Tick() noexcept
{
    if (!Watching())
    {
        return;
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
            platform::LogDebug(L"enemy watch: {} {:#x} is in the battle: {} on {}", r.name, key, ActionName(action), CharacterName(r.character));
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
        anyone = true;
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
                platform::LogDebug(L"enemy watch: {} {:#x} starts {} on {}", r.name, key, ActionName(action), CharacterName(r.character));
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
                platform::LogDebug(L"enemy watch: {} {:#x} now targets {} ({})", r.name, key, CharacterName(character),
                                   action != 0 ? ActionName(action) : std::wstring(L"between actions"));
            }
        }
        if (action == 0 && now >= r.nextWaitMs)
        {
            platform::LogDebug(L"enemy watch: {} {:#x} has started no action for {:.1f} s (target {})", r.name, key, Seconds(now - r.sinceMs),
                               CharacterName(TargetOf(live)));
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
