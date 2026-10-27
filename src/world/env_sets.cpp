#include "world/env_sets.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/seats.h"
#include "world/actor_place.h"
#include "world/env_set_view.h"

#include <Unreal/NameTypes.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <utility>

namespace lss::world::env_sets {

namespace {

namespace view = env_set_view;
using view::Entry;
using view::Name;
using platform::Read;
using platform::ReadPointer;
using players::kSeatsBuilt;
using players::SeatBuilt;
using players::SeatName;

constexpr int kMaxPlayers = players::scopes::kMaxPlayers;

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
using Ids    = std::array<std::uint64_t, 3>;

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

// Game thread only: the ticks, and the two calls inside the manager's evaluation.
bool g_enabled = true;
bool g_placed  = false;

/// One further player's set and view, everything the module keeps per seat.
struct Seat
{
    void*         member     = nullptr;  // the seat's character while in
    std::uint8_t* controller = nullptr;  // and controller
    std::uint64_t handle     = 0;        // the seat's trace, readable the frame after
    alignas(16) std::array<std::uint8_t, facts::kEnvSetBlockBytes> block{};  // the seat's set (zero = constructed)
    view::TwoSet fresh;                  // this frame's read
    view::TwoSet kept;                   // the last set read (kept through a no-hit)
    view::Paced  paced;                  // the set the seat's view follows
    Ids          ids{};                  // the kept set's ids (the reading and the log)
    Ids          pacedIds{};
    bool         due        = false;     // the view's list is to be looked up again
    bool         lookedUp   = false;     // a lookup has run (zero-based with lookedUpAt, so the records cost the DLL nothing)
    double       lookedUpAt = 0.0;
    Actors        wanted;                // this lookup's actors
    Actors        ours;                  // the actors the mod put in oursIn's list
    std::uint8_t* oursIn = nullptr;
    std::uint64_t traces = 0, reads = 0, changes = 0, writes = 0;
    int           notFound = 0, hiddenEntries = 0, shownButHidden = 0, attachedCount = 0;
};
std::array<Seat, kMaxPlayers> g_seats;  // by seat; [0] unused: all zero at start, so the records cost the DLL nothing
// The last thing that stopped each player's part (logged on change). Apart from the records: a string's empty state
// is not all zero, and one inside the record would put every record into the DLL's data (0.55 MB, measured).
std::array<std::wstring, kMaxPlayers> g_problems;

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

std::wstring IdsText(const Ids& ids)
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

/// The last thing that stopped the seat's part, formed and kept inside the guard; logged on change.
template <class... A>
void Problem(int seat, std::wformat_string<A...> text, A&&... args) noexcept
try
{
    std::wstring  what    = std::format(text, std::forward<A>(args)...);
    std::wstring& problem = g_problems[static_cast<std::size_t>(seat)];
    if (problem != what)
    {
        problem = std::move(what);
        platform::LogDebug(L"env sets: {}: {}", SeatName(seat), problem);
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
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        Seat& s = g_seats[seat];
        if (s.member == nullptr)
        {
            s.handle = 0;
            continue;
        }
        TraceArg      own{};
        std::int32_t  tags = 0;
        if (!actor_place::ActorLocation(s.member, own.location))
        {
            s.handle = 0;  // no place to trace from (the game would trace from the origin; the mod skips it)
            continue;
        }
        if (Read(static_cast<std::uint8_t*>(s.member) + facts::kActorTags + 8, tags) && tags > 0)
        {
            Read(ReadPointer(s.member, facts::kActorTags), own.tag);
        }
        g_issue(nullptr, &own, 0, 0, &s.handle);
        ++s.traces;
    }
    return result;
}

// --- 2. The read ---------------------------------------------------------------------------------------------------

enum class ReadResult
{
    NoSet,     // no set for this seat this frame (no trace, no ground naming one, or unreadable)
    Set,       // the seat's set is in s.fresh
    NameLost,  // the save data's set name could not be put back: no further seat may blank it this frame
};

/// The game's read of the seat's trace into the mod's block (s.fresh), with the save data's set name kept player 1's.
ReadResult ReadSeat(int seat, Seat& s, void* manager) noexcept
{
    if (s.handle == 0)
    {
        return ReadResult::NoSet;
    }
    std::uint8_t* save  = nullptr;
    std::int32_t  state = 0;
    if (!Read(g_save, save) || save == nullptr || !Read(save + facts::kSaveDataState, state))
    {
        Problem(seat, L"the save data is not there; the set is not read");
        return ReadResult::NoSet;
    }
    std::uint8_t* const name = state == facts::kSaveDataStateActive
                                   ? save + facts::kSaveDataActive + facts::kSaveDataEnvSetName
                                   : nullptr;
    std::array<std::uint8_t, facts::kSaveDataEnvSetNameBytes> savedName{};
    constexpr std::uint8_t kBlank = 0;
    if (name != nullptr && (!platform::CopyGuarded(savedName.data(), name, savedName.size()) ||
                            !platform::WriteGuarded(name, &kBlank, 1)))
    {
        Problem(seat, L"the save data's set name could not be kept; the set is not read");
        return ReadResult::NoSet;
    }
    g_reset(s.block.data());
    const char read = g_read(manager, &s.handle, s.block.data());
    ++s.reads;
    if (name != nullptr && !platform::WriteGuarded(name, savedName.data(), savedName.size()))
    {
        platform::LogWarning(L"env sets: the save data's set name could not be put back");
        Problem(seat, L"the save data's set name could not be put back; no other player's set is read this frame");
        return ReadResult::NameLost;
    }
    if (read == 0)
    {
        return ReadResult::NoSet;  // no ground under the player that names a set (or the trace not back yet)
    }
    Entry*       entries = nullptr;
    const Name*  levels  = nullptr;
    std::int32_t entryCount = 0, levelCount = 0;
    if (!Entries(s.block.data(), entries, entryCount) || !Levels(s.block.data(), levels, levelCount))
    {
        return ReadResult::NoSet;
    }
    if (entryCount > view::kMaxEntries || levelCount > view::kMaxLevels)
    {
        Problem(seat, L"the set lists {} actors and {} levels, more than the mod holds ({}, {}); kept the last one",
                entryCount, levelCount, view::kMaxEntries, view::kMaxLevels);
        return ReadResult::NoSet;
    }
    s.fresh.entries.count = 0;
    s.fresh.levels.count  = 0;
    for (std::int32_t i = 0; i < entryCount; ++i)
    {
        s.fresh.entries.Add(entries[i]);
    }
    for (std::int32_t i = 0; i < levelCount; ++i)
    {
        s.fresh.levels.Add(levels[i]);
    }
    Ids ids{};
    platform::CopyGuarded(ids.data(), s.block.data() + facts::kEnvSetBlockIds, sizeof(ids));
    if (ids != s.ids)
    {
        platform::LogDebug(L"env sets: {}'s ground names set {} ({} actors, {} levels)", SeatName(seat), IdsText(ids),
                           s.fresh.entries.count, s.fresh.levels.count);
        s.ids = ids;
    }
    return ReadResult::Set;
}

// --- 3. The seat's view ---------------------------------------------------------------------------------------------

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

/// The levels of the seat's set the game's apply would search: those in the manager's lookup set.
void LookupLevels(std::uint8_t* manager, const Seat& s) noexcept
{
    void* const lookup = manager + facts::kEnvSetMgrLookupLevels;
    g_lookupLevels.count = 0;
    for (const Name level : s.paced.set.levels.View())
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
void AddAttached(Seat& s, void* actor) noexcept
{
    PointerArray out{nullptr, 0, 0};
    if (platform::CallGuarded(g_attached, actor, &out) && out.data != nullptr)
    {
        for (std::int32_t i = 0; i < out.num; ++i)
        {
            void* child = nullptr;
            if (Read(out.data + i, child) && child != nullptr && AddOnce(s.wanted, child))
            {
                ++s.attachedCount;
            }
        }
    }
    if (out.data != nullptr)
    {
        platform::CallGuarded(g_free, static_cast<void*>(out.data));
    }
}

/// The actors the seat's view hides now (s.wanted), with the counts the reading shows.
bool LookUp(int seat, Seat& s, std::uint8_t* manager) noexcept
{
    s.wanted.count   = 0;
    s.notFound       = 0;
    s.hiddenEntries  = 0;
    s.shownButHidden = 0;
    s.attachedCount  = 0;
    LookupLevels(manager, s);
    for (const Entry& entry : s.paced.set.entries.View())
    {
        std::uint8_t* const actor = Find(entry.name);
        if (entry.hidden == 0)
        {
            // Shown in the player's single player; a hide of player 1's cannot be undone for one view alone.
            std::uint8_t flags = 0;
            if (actor != nullptr && Read(actor + facts::kActorHiddenFlags, flags) && (flags & facts::kActorHiddenBit) != 0)
            {
                ++s.shownButHidden;
            }
            continue;
        }
        ++s.hiddenEntries;
        if (actor == nullptr)
        {
            ++s.notFound;  // not loaded or not spawned yet: looked up again at the next refresh
            continue;
        }
        if (!AddOnce(s.wanted, actor))
        {
            Problem(seat, L"more actors to hide in the view than the mod holds; the rest stay");
            return false;
        }
        AddAttached(s, actor);
    }
    return true;
}

/// The seat's controller's HiddenActors: the mod's earlier entries out, this lookup's in; other code's entries kept.
bool Write(int seat, Seat& s, std::uint8_t* controller) noexcept
{
    auto* const  list = reinterpret_cast<PointerArray*>(controller + facts::kControllerHiddenActors);
    PointerArray now{};
    if (!Read(list, now) || now.num < 0 || now.max < now.num || (now.num > 0 && now.data == nullptr))
    {
        Problem(seat, L"the hidden-actor list is not readable");
        return false;
    }
    if (s.oursIn != controller)
    {
        s.ours.count = 0;  // a new controller: nothing of the mod's there
        std::uint8_t renders = 0;
        Read(controller + facts::kControllerRenderPrimitives, renders);
        platform::LogDebug(L"env sets: {}'s controller builds its view's hidden list {} (bRenderPrimitiveComponents {}); "
                           L"{} actors listed before the mod's",
                           SeatName(seat), renders != 0 ? L"yes" : L"NO", renders, now.num);
    }
    // The engine's own list memory, on the game thread, which also builds the views from it.
    std::int32_t kept = now.num > 0 ? view::RemoveOurs(now.data, now.num, s.ours.View()) : 0;
    const std::int32_t needed = kept + s.wanted.count;
    if (needed > now.max)
    {
        const std::int32_t num = kept;
        platform::WriteGuarded(&list->num, &num, sizeof(num));
        if (!platform::CallGuarded(g_resize, list, needed) || !Read(list, now) || now.max < needed || now.data == nullptr)
        {
            s.ours.count = 0;
            Problem(seat, L"the hidden-actor list could not grow; nothing hidden in the view");
            return false;
        }
    }
    std::memcpy(now.data + kept, s.wanted.items.data(), sizeof(void*) * static_cast<std::size_t>(s.wanted.count));
    platform::WriteGuarded(&list->num, &needed, sizeof(needed));
    s.ours   = s.wanted;
    s.oursIn = controller;
    ++s.writes;
    return true;
}

void Refresh(int seat, Seat& s, std::uint8_t* manager, bool changed) noexcept
{
    const double now = Now();
    if (s.controller == nullptr || (!changed && !s.due && s.lookedUp && now - s.lookedUpAt < kRefresh))
    {
        return;
    }
    s.due        = false;
    s.lookedUp   = true;
    s.lookedUpAt = now;
    if (!LookUp(seat, s, manager) || (s.oursIn == s.controller && s.wanted == s.ours))
    {
        return;
    }
    if (Write(seat, s, s.controller))
    {
        platform::LogDebug(L"env sets: {}'s view hides {} actors of set {} ({} attached; {} of its {} hidden entries not "
                           L"found; {} it shows are hidden by player 1's set)",
                           SeatName(seat), s.ours.count, IdsText(s.pacedIds), s.attachedCount, s.notFound, s.hiddenEntries,
                           s.shownButHidden);
        platform::LogDebug(L"env sets: searched {} of the set's {} levels (the ones in the game's lookup set)",
                           g_lookupLevels.count, s.paced.set.levels.count);
    }
}

char ReadSet(void* manager, const std::uint64_t* handle, std::uint8_t* block) noexcept
{
    const char   one  = g_read(manager, handle, block);  // player 1's, as the game reads it
    std::uint8_t hold = 0;
    const bool   holding = Read(static_cast<std::uint8_t*>(manager) + facts::kEnvSetMgrHoldMode, hold) && hold != 0;
    bool nameLost = false;  // the save data's set name could not be put back: no further seat reads this frame
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        Seat& s = g_seats[seat];
        // Hold mode: one scripted set for everyone, nothing for a further player alone. Off or out: nothing either.
        const bool       present = s.member != nullptr && g_enabled && !holding;
        const ReadResult read    = present && !nameLost ? ReadSeat(seat, s, manager) : ReadResult::NoSet;
        const bool       resolved = read == ReadResult::Set;
        nameLost                  = nameLost || read == ReadResult::NameLost;
        if (!present)
        {
            s.ids = {};
        }
        const view::TwoSet& next = view::NextTwoSet(present, resolved, s.fresh, s.kept);
        if (&next != &s.kept && !(next == s.kept))
        {
            s.kept = next;
        }
        const bool changed = view::Pace(s.paced, s.kept, Now());
        if (changed)
        {
            ++s.changes;
            s.pacedIds = present ? s.ids : Ids{};
        }
        Refresh(seat, s, static_cast<std::uint8_t*>(manager), changed);
    }
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
        platform::LogWarning(L"env sets: the EnvironmentSet code is not what the research read; every further player's "
                             L"view shows what player 1's ground leaves");
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
        platform::LogWarning(L"env sets: the read could not be redirected; every further player's view shows what player "
                             L"1's ground leaves");
        return false;
    }
    g_issueSite   = *issueCall;
    g_issueTarget = *issue;
    control::Register({.name  = L"env-sets",
                       .shape = control::Shape::OnOff,
                       .help  = L"each further player's view hides what the area hides under that player, as player 1's does "
                                L"(world/env_sets.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"world.env-sets", .shape = control::Shape::Text,
                       .help = L"each further player's EnvironmentSet and the actors hidden in their view",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"env sets: the read is redirected; the further players' traces are placed on the first tick; each "
                  L"further player's view hides what their ground hides (the world stays player 1's)");
    return true;
}

void TickSeat(int seat, void* seatMember, void* seatController) noexcept
{
    if (!SeatBuilt(seat))
    {
        return;
    }
    Seat&               s          = g_seats[seat];
    std::uint8_t* const controller = seatMember != nullptr ? static_cast<std::uint8_t*>(seatController) : nullptr;
    if (controller != s.controller)
    {
        s.due = true;  // a join (a new controller): its list is filled at the next read
    }
    s.member     = seatMember;
    s.controller = controller;
}

void Tick() noexcept
{
    if (g_placed || g_issueSite == 0)
    {
        return;
    }
    g_placed = true;  // once, whatever the outcome; outside the world tick, as the crossing call needs
    if (g_issueCall.InstallFromOwnThread(g_issueSite, g_issueTarget, reinterpret_cast<std::uintptr_t>(&Issue)))
    {
        platform::Log(L"env sets: the trace is redirected; each further player's view hides what their ground hides");
    }
    else
    {
        platform::LogWarning(L"env sets: the trace could not be redirected; every further player's view shows what player "
                             L"1's ground leaves");
    }
}

void SetEnabled(bool on) noexcept
{
    g_enabled = on;
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        g_seats[seat].due = true;
    }
    platform::Log(L"env sets {}", on ? L"on" : L"off (a further player's view hides nothing of its own from the next read)");
}

std::wstring Reading() noexcept
try
{
    Ids                 oneIds{};
    std::uint8_t* const manager = g_manager != nullptr ? *g_manager : nullptr;
    if (manager != nullptr)
    {
        platform::CopyGuarded(oneIds.data(), manager + facts::kEnvSetMgrSetIds, sizeof(oneIds));
    }
    std::wstring text = std::format(L"{}; trace {}; player 1's set {}", g_enabled ? L"on" : L"off",
                                    g_issueCall.IsInstalled() ? L"placed" : L"not placed", IdsText(oneIds));
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[seat];
        text += std::format(L"; {}: ground {}; view follows {} (changes {}): hides {} actors ({} attached), {} of {} hidden "
                            L"entries not found, {} shown entries hidden by player 1's set; list writes {}; traces {}, reads {}{}",
                            SeatName(seat), IdsText(s.ids), IdsText(s.pacedIds), s.changes, s.ours.count, s.attachedCount,
                            s.notFound, s.hiddenEntries, s.shownButHidden, s.writes, s.traces, s.reads,
                            g_problems[static_cast<std::size_t>(seat)].empty()
                                ? std::wstring{}
                                : L"; last problem: " + g_problems[static_cast<std::size_t>(seat)]);
    }
    return text;
}
catch (...)
{
    return L"?";
}

}  // namespace lss::world::env_sets
