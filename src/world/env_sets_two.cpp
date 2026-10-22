#include "world/env_sets_two.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "world/env_set_view.h"

#include <Unreal/NameTypes.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>

namespace lss::world::env_sets_two {

namespace {

namespace view = env_set_view;
using view::Entry;
using view::Name;
using platform::Read;
using platform::ReadPointer;

/// What the trace function reads: the owner tag (the character's first tag, for the engine's trace statistics) and
/// the place to trace down from. The game passes a 0x18-byte slot.
struct TraceArg
{
    Name  tag;
    float location[3];
    float pad;
};
static_assert(sizeof(TraceArg) == 0x18);

/// A TArray of pointers as the engine lays it out.
struct PointerArray
{
    void**       data;
    std::int32_t num;
    std::int32_t max;
};
static_assert(sizeof(PointerArray) == 0x10);

using IssueFn     = char (*)(void* unused, const TraceArg* arg, std::uint64_t, std::uint64_t, std::uint64_t* handle);
using ReadFn      = char (*)(void* manager, const std::uint64_t* handle, std::uint8_t* block);
using ResetFn     = void (*)(std::uint8_t* block);
using FindIdFn    = std::int32_t* (*)(void* set, std::int32_t* out, std::uint64_t name);
using FindActorFn = std::uint8_t* (*)(std::uint64_t level, std::uint64_t name);
using ResizeFn    = void (*)(PointerArray* array, std::int32_t max);
using AttachedFn  = void (*)(void* actor, PointerArray* out);
using FreeFn      = void (*)(void* block);

using Actors = view::List<void*, view::kMaxHidden>;

constexpr double kRefresh = 1.0;  // seconds between lookups while a set stands: actors that spawn or leave later

platform::CallRedirect g_issueCall, g_readCall;
std::uintptr_t         g_issueSite = 0, g_issueTarget = 0;
IssueFn                g_issue     = nullptr;
ReadFn                 g_read      = nullptr;
ResetFn                g_reset     = nullptr;
FindIdFn               g_findId    = nullptr;
FindActorFn            g_findActor = nullptr;
ResizeFn               g_resize    = nullptr;
AttachedFn             g_attached  = nullptr;
FreeFn                 g_free      = nullptr;
std::uint8_t**         g_save      = nullptr;  // &the save data singleton
std::uint8_t**         g_manager   = nullptr;  // &the manager (the reading)

// Game thread only: Tick, and the two calls inside the manager's evaluation.
bool          g_enabled       = true;
bool          g_placed        = false;
void*         g_two           = nullptr;  // player 2's character while in
std::uint8_t* g_twoController = nullptr;  // and controller
std::uint64_t g_twoHandle     = 0;        // player 2's trace, readable the frame after
alignas(16) std::array<std::uint8_t, facts::kEnvSetBlockBytes> g_twoBlock{};  // player 2's set (zero = constructed)
view::TwoSet  g_fresh;                    // this frame's read
view::TwoSet  g_kept;                     // the last set read (kept through a no-hit)
view::Paced   g_paced;                    // the set player 2's view follows
std::array<std::uint64_t, 3> g_twoIds{};  // the kept set's ids (the reading and the log)
std::array<std::uint64_t, 3> g_pacedIds{};
bool          g_due           = false;    // the view's list is to be looked up again
double        g_lookedUpAt    = -1.0e300;

Actors        g_wanted;                   // this lookup's actors
Actors        g_ours;                     // the actors the mod put in g_oursIn's list
std::uint8_t* g_oursIn        = nullptr;

std::uint64_t g_traces = 0, g_reads = 0, g_changes = 0, g_writes = 0;
int           g_notFound = 0, g_hiddenEntries = 0, g_shownButHidden = 0, g_attachedCount = 0;
std::wstring  g_problem;                  // the last thing that stopped player 2's part (logged on change)

double Now() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::uint64_t Bits(Name name) noexcept
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &name, sizeof(bits));
    return bits;
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

std::wstring Text(Name name)
{
    std::wstring text;
    return platform::CallGuarded(&NameText, static_cast<std::int64_t>(Bits(name)), &text) && !text.empty() ? text : L"?";
}

std::wstring IdsText(const std::array<std::uint64_t, 3>& ids)
{
    std::wstring out;
    for (const std::uint64_t id : ids)
    {
        if (id != 0)
        {
            Name name;
            std::memcpy(&name, &id, sizeof(name));
            out += (out.empty() ? L"" : L" ") + Text(name);
        }
    }
    return out.empty() ? L"none" : out;
}

void Problem(std::wstring_view what) noexcept
try
{
    if (g_problem != what)
    {
        g_problem = what;
        if (!what.empty())
        {
            platform::LogDebug(L"env sets 2: {}", what);
        }
    }
}
catch (...)
{
}

/// A block's actor list: the heap data when grown, else the inline data.
bool Entries(std::uint8_t* block, Entry*& data, std::int32_t& num) noexcept
{
    auto* heap = static_cast<Entry*>(ReadPointer(block, facts::kEnvSetBlockActorsHeap));
    data       = heap != nullptr ? heap : reinterpret_cast<Entry*>(block + facts::kEnvSetBlockActors);
    return Read(block + facts::kEnvSetBlockActorsNum, num) && num >= 0;
}

bool Levels(std::uint8_t* block, const Name*& data, std::int32_t& num) noexcept
{
    data = static_cast<const Name*>(ReadPointer(block, facts::kEnvSetBlockLevels));
    return Read(block + facts::kEnvSetBlockLevels + 8, num) && num >= 0 && (num == 0 || data != nullptr);
}

// --- 1. The trace --------------------------------------------------------------------------------------------------

char Issue(void* unused, const TraceArg* arg, std::uint64_t r8, std::uint64_t r9, std::uint64_t* handle) noexcept
{
    const char result = g_issue(unused, arg, r8, r9, handle);  // player 1's, as the game issues it
    if (g_two == nullptr)
    {
        g_twoHandle = 0;
        return result;
    }
    TraceArg      two{};
    std::int32_t  tags = 0;
    auto* const   root = static_cast<std::uint8_t*>(ReadPointer(g_two, facts::kActorRootComponent));
    if (root == nullptr || !Read(root + facts::kComponentWorldLocation, two.location))
    {
        g_twoHandle = 0;  // no place to trace from (the game would trace from the origin; the mod skips it)
        return result;
    }
    if (Read(static_cast<std::uint8_t*>(g_two) + facts::kActorTags + 8, tags) && tags > 0)
    {
        Read(ReadPointer(g_two, facts::kActorTags), two.tag);
    }
    g_issue(nullptr, &two, 0, 0, &g_twoHandle);
    ++g_traces;
    return result;
}

// --- 2. The read ---------------------------------------------------------------------------------------------------

/// The game's read of player 2's trace into the mod's block (g_fresh), with the save data's set name kept player 1's.
bool ReadTwo(void* manager) noexcept
{
    if (g_twoHandle == 0)
    {
        return false;
    }
    std::uint8_t* save  = nullptr;
    std::int32_t  state = 0;
    if (!Read(g_save, save) || save == nullptr || !Read(save + facts::kSaveDataState, state))
    {
        Problem(L"the save data is not there; player 2's set is not read");
        return false;
    }
    std::uint8_t* const name = state == facts::kSaveDataStateActive
                                   ? save + facts::kSaveDataActive + facts::kSaveDataEnvSetName
                                   : nullptr;
    std::array<std::uint8_t, facts::kSaveDataEnvSetNameBytes> kept{};
    constexpr std::uint8_t kBlank = 0;
    if (name != nullptr && (!platform::CopyGuarded(kept.data(), name, kept.size()) ||
                            !platform::WriteGuarded(name, &kBlank, 1)))
    {
        Problem(L"the save data's set name could not be kept; player 2's set is not read");
        return false;
    }
    g_reset(g_twoBlock.data());
    const char two = g_read(manager, &g_twoHandle, g_twoBlock.data());
    if (name != nullptr && !platform::WriteGuarded(name, kept.data(), kept.size()))
    {
        platform::LogWarning(L"env sets 2: the save data's set name could not be put back");
    }
    ++g_reads;
    if (two == 0)
    {
        return false;  // no ground under player 2 that names a set (or the trace not back yet)
    }
    Entry*       entries = nullptr;
    const Name*  levels  = nullptr;
    std::int32_t entryCount = 0, levelCount = 0;
    if (!Entries(g_twoBlock.data(), entries, entryCount) || !Levels(g_twoBlock.data(), levels, levelCount))
    {
        return false;
    }
    if (entryCount > view::kMaxEntries || levelCount > view::kMaxLevels)
    {
        Problem(std::format(L"player 2's set lists {} actors and {} levels, more than the mod holds ({}, {}); kept the "
                            L"last one",
                            entryCount, levelCount, view::kMaxEntries, view::kMaxLevels));
        return false;
    }
    g_fresh.entries.count = 0;
    g_fresh.levels.count  = 0;
    for (std::int32_t i = 0; i < entryCount; ++i)
    {
        g_fresh.entries.Add(entries[i]);
    }
    for (std::int32_t i = 0; i < levelCount; ++i)
    {
        g_fresh.levels.Add(levels[i]);
    }
    std::array<std::uint64_t, 3> ids{};
    platform::CopyGuarded(ids.data(), g_twoBlock.data() + facts::kEnvSetBlockIds, sizeof(ids));
    if (ids != g_twoIds)
    {
        platform::LogDebug(L"env sets 2: player 2's ground names set {} ({} actors, {} levels)", IdsText(ids),
                           g_fresh.entries.count, g_fresh.levels.count);
        g_twoIds = ids;
    }
    return true;
}

// --- 3. Player 2's view --------------------------------------------------------------------------------------------

bool AddOnce(Actors& list, void* actor) noexcept
{
    for (void* const held : list.View())
    {
        if (held == actor)
        {
            return true;
        }
    }
    return list.Add(actor);
}

view::LevelList g_lookupLevels;  // the set's levels that are in the manager's lookup set (this lookup)

/// The levels of player 2's set the game's apply would search: those in the manager's lookup set.
void LookupLevels(std::uint8_t* manager) noexcept
{
    void* const lookup = manager + facts::kEnvSetMgrLookupLevels;
    g_lookupLevels.count = 0;
    for (const Name level : g_paced.set.levels.View())
    {
        std::int32_t  index = -1;
        std::int32_t* found = nullptr;
        if (platform::CallGuarded(g_findId, found, lookup, &index, Bits(level)) && index != -1)
        {
            g_lookupLevels.Add(level);
        }
    }
}

/// The actor a set entry names, found as the game's apply finds it: in those levels, by the character registry's
/// per-level name map.
std::uint8_t* Find(Name name) noexcept
{
    for (const Name level : g_lookupLevels.View())
    {
        std::uint8_t* actor = nullptr;
        if (platform::CallGuarded(g_findActor, actor, Bits(level), Bits(name)) && actor != nullptr)
        {
            return actor;
        }
    }
    return nullptr;
}

/// Every actor attached below `actor` (the game's own hide reaches them through their parent's reasons; the view list
/// follows one attach level only, so they are listed themselves).
void AddAttached(void* actor) noexcept
{
    PointerArray out{nullptr, 0, 0};
    if (platform::CallGuarded(g_attached, actor, &out) && out.data != nullptr)
    {
        for (std::int32_t i = 0; i < out.num; ++i)
        {
            void* child = nullptr;
            if (Read(out.data + i, child) && child != nullptr && AddOnce(g_wanted, child))
            {
                ++g_attachedCount;
            }
        }
    }
    if (out.data != nullptr)
    {
        platform::CallGuarded(g_free, static_cast<void*>(out.data));
    }
}

/// The actors player 2's view hides now (g_wanted), with the counts the reading shows.
bool LookUp(std::uint8_t* manager) noexcept
{
    g_wanted.count  = 0;
    g_notFound      = 0;
    g_hiddenEntries = 0;
    g_shownButHidden = 0;
    g_attachedCount = 0;
    LookupLevels(manager);
    for (const Entry& entry : g_paced.set.entries.View())
    {
        std::uint8_t* const actor = Find(entry.name);
        if (entry.hidden == 0)
        {
            // Shown in player 2's single player; a hide of player 1's cannot be undone for one view alone.
            std::uint8_t flags = 0;
            if (actor != nullptr && Read(actor + facts::kActorHiddenFlags, flags) && (flags & facts::kActorHiddenBit) != 0)
            {
                ++g_shownButHidden;
            }
            continue;
        }
        ++g_hiddenEntries;
        if (actor == nullptr)
        {
            ++g_notFound;  // not loaded or not spawned yet: looked up again at the next refresh
            continue;
        }
        if (!AddOnce(g_wanted, actor))
        {
            Problem(L"more actors to hide in player 2's view than the mod holds; the rest stay");
            return false;
        }
        AddAttached(actor);
    }
    return true;
}

/// Player 2's controller's HiddenActors: the mod's earlier entries out, this lookup's in; other code's entries kept.
bool Write(std::uint8_t* controller) noexcept
{
    auto* const  list = reinterpret_cast<PointerArray*>(controller + facts::kControllerHiddenActors);
    PointerArray now{};
    if (!Read(list, now) || now.num < 0 || now.max < now.num || (now.num > 0 && now.data == nullptr))
    {
        Problem(L"player 2's hidden-actor list is not readable");
        return false;
    }
    if (g_oursIn != controller)
    {
        g_ours.count = 0;  // a new controller: nothing of the mod's there
        std::uint8_t renders = 0;
        Read(controller + facts::kControllerRenderPrimitives, renders);
        platform::LogDebug(L"env sets 2: player 2's controller builds its view's hidden list {} (bRenderPrimitiveComponents "
                           L"{}); {} actors listed before the mod's", renders != 0 ? L"yes" : L"NO", renders, now.num);
    }
    // The engine's own list memory, on the game thread, which also builds the views from it.
    std::int32_t kept = now.num > 0 ? view::RemoveOurs(now.data, now.num, g_ours.View()) : 0;
    const std::int32_t needed = kept + g_wanted.count;
    if (needed > now.max)
    {
        const std::int32_t num = kept;
        platform::WriteGuarded(&list->num, &num, sizeof(num));
        if (!platform::CallGuarded(g_resize, list, needed) || !Read(list, now) || now.max < needed || now.data == nullptr)
        {
            g_ours.count = 0;
            Problem(L"player 2's hidden-actor list could not grow; nothing hidden in player 2's view");
            return false;
        }
    }
    std::memcpy(now.data + kept, g_wanted.items.data(), sizeof(void*) * static_cast<std::size_t>(g_wanted.count));
    platform::WriteGuarded(&list->num, &needed, sizeof(needed));
    g_ours   = g_wanted;
    g_oursIn = controller;
    ++g_writes;
    return true;
}

void Refresh(std::uint8_t* manager, bool changed) noexcept
{
    const double now = Now();
    if (g_twoController == nullptr || (!changed && !g_due && now - g_lookedUpAt < kRefresh))
    {
        return;
    }
    g_due        = false;
    g_lookedUpAt = now;
    if (!LookUp(manager) || (g_oursIn == g_twoController && g_wanted == g_ours))
    {
        return;
    }
    if (Write(g_twoController))
    {
        platform::LogDebug(L"env sets 2: player 2's view hides {} actors of set {} ({} attached; {} of its {} hidden "
                           L"entries not found; {} it shows are hidden by player 1's set)",
                           g_ours.count, IdsText(g_pacedIds), g_attachedCount, g_notFound, g_hiddenEntries,
                           g_shownButHidden);
        platform::LogDebug(L"env sets 2: searched {} of the set's {} levels (the ones in the game's lookup set)",
                           g_lookupLevels.count, g_paced.set.levels.count);
    }
}

char ReadSet(void* manager, const std::uint64_t* handle, std::uint8_t* block) noexcept
{
    const char   one  = g_read(manager, handle, block);  // player 1's, as the game reads it
    std::uint8_t hold = 0;
    const bool   holding = Read(static_cast<std::uint8_t*>(manager) + facts::kEnvSetMgrHoldMode, hold) && hold != 0;
    // Hold mode: one scripted set for everyone, nothing for player 2 alone. Off or out: nothing either.
    const bool present  = g_two != nullptr && g_enabled && !holding;
    const bool resolved = present && ReadTwo(manager);
    if (!present)
    {
        g_twoIds = {};
    }
    const view::TwoSet& next = view::NextTwoSet(present, resolved, g_fresh, g_kept);
    if (&next != &g_kept && !(next == g_kept))
    {
        g_kept = next;
    }
    const bool changed = view::Pace(g_paced, g_kept, Now());
    if (changed)
    {
        ++g_changes;
        g_pacedIds = present ? g_twoIds : std::array<std::uint64_t, 3>{};
    }
    Refresh(static_cast<std::uint8_t*>(manager), changed);
    return one;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto issueCall = image.Rebase(facts::kEnvSetIssueTraceCall, 5);
    const auto readCall  = image.Rebase(facts::kEnvSetReadResultCall, 5);
    const auto issue     = platform::FindFunction(image, facts::kEnvSetIssueTrace);
    const auto read      = platform::FindFunction(image, facts::kEnvSetReadResult);
    const auto reset     = platform::FindFunction(image, facts::kEnvSetBlockReset);
    const auto findId    = platform::FindFunction(image, facts::kTSetNameFind);
    const auto findActor = platform::FindFunction(image, facts::kFindActorByName);
    const auto resize    = platform::FindFunction(image, facts::kPointerArrayResize);
    const auto attached  = platform::FindFunction(image, facts::kGetAttachedActors);
    const auto release   = platform::FindFunction(image, facts::kGameFree);
    const auto save      = image.Rebase(facts::kSaveDataGlobal, sizeof(void*));
    const auto manager   = image.Rebase(facts::kEnvSetManagerGlobal, sizeof(void*));
    if (!issueCall || !readCall || !issue || !read || !reset || !findId || !findActor || !resize || !attached ||
        !release || !save || !manager)
    {
        platform::LogWarning(L"env sets 2: the EnvironmentSet code is not what the research read; player 2's view shows "
                             L"what player 1's ground leaves");
        return false;
    }
    g_issue     = reinterpret_cast<IssueFn>(*issue);
    g_read      = reinterpret_cast<ReadFn>(*read);
    g_reset     = reinterpret_cast<ResetFn>(*reset);
    g_findId    = reinterpret_cast<FindIdFn>(*findId);
    g_findActor = reinterpret_cast<FindActorFn>(*findActor);
    g_resize    = reinterpret_cast<ResizeFn>(*resize);
    g_attached  = reinterpret_cast<AttachedFn>(*attached);
    g_free      = reinterpret_cast<FreeFn>(*release);
    g_save      = reinterpret_cast<std::uint8_t**>(*save);
    g_manager   = reinterpret_cast<std::uint8_t**>(*manager);
    if (!g_readCall.Install(*readCall, *read, reinterpret_cast<std::uintptr_t>(&ReadSet)))
    {
        platform::LogWarning(L"env sets 2: the read could not be redirected; player 2's view shows what player 1's "
                             L"ground leaves");
        return false;
    }
    g_issueSite   = *issueCall;
    g_issueTarget = *issue;
    control::Register({.name  = L"env-sets",
                       .shape = control::Shape::OnOff,
                       .help  = L"player 2's view hides what the area hides under player 2, as player 1's does "
                                L"(world/env_sets_two.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"world.env-sets", .shape = control::Shape::Text,
                       .help = L"player 2's EnvironmentSet and the actors hidden in player 2's view",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"env sets 2: the read is redirected; player 2's trace is placed on the first tick; player 2's view "
                  L"hides what player 2's ground hides (the world stays player 1's)");
    return true;
}

void Tick(void* memberTwo, void* controllerTwo) noexcept
{
    std::uint8_t* const controller = memberTwo != nullptr ? static_cast<std::uint8_t*>(controllerTwo) : nullptr;
    if (controller != g_twoController)
    {
        g_due = true;  // a join (a new controller): its list is filled at the next read
    }
    g_two           = memberTwo;
    g_twoController = controller;
    if (g_placed || g_issueSite == 0)
    {
        return;
    }
    g_placed = true;  // once, whatever the outcome; outside the world tick, as the crossing call needs
    if (g_issueCall.InstallFromOwnThread(g_issueSite, g_issueTarget, reinterpret_cast<std::uintptr_t>(&Issue)))
    {
        platform::Log(L"env sets 2: the trace is redirected; player 2's view hides what player 2's ground hides");
    }
    else
    {
        platform::LogWarning(L"env sets 2: the trace could not be redirected; player 2's view shows what player 1's "
                             L"ground leaves");
    }
}

void SetEnabled(bool on) noexcept
{
    g_enabled = on;
    g_due     = true;
    platform::Log(L"env sets 2 {}", on ? L"on" : L"off (player 2's view hides nothing of its own from the next read)");
}

std::wstring Reading() noexcept
try
{
    std::array<std::uint64_t, 3> oneIds{};
    std::uint8_t* const manager = g_manager != nullptr ? *g_manager : nullptr;
    if (manager != nullptr)
    {
        platform::CopyGuarded(oneIds.data(), manager + facts::kEnvSetMgrSetIds, sizeof(oneIds));
    }
    return std::format(L"{}; trace {}; player 1's set {}; player 2's ground {}; player 2's view follows {} (changes {}): "
                       L"hides {} actors ({} attached), {} of {} hidden entries not found, {} shown entries hidden by "
                       L"player 1's set; list writes {}; player 2's traces {}, reads {}{}",
                       g_enabled ? L"on" : L"off", g_issueCall.IsInstalled() ? L"placed" : L"not placed",
                       IdsText(oneIds), IdsText(g_twoIds), IdsText(g_pacedIds), g_changes, g_ours.count, g_attachedCount,
                       g_notFound, g_hiddenEntries, g_shownButHidden, g_writes, g_traces, g_reads,
                       g_problem.empty() ? std::wstring{} : L"; last problem: " + g_problem);
}
catch (...)
{
    return L"?";
}

}  // namespace lss::world::env_sets_two
