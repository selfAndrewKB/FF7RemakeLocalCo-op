#include "world/env_sets_two.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "world/env_set_merge.h"

#include <Unreal/NameTypes.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>

namespace lss::world::env_sets_two {

namespace {

namespace merge = env_set_merge;
using merge::Entry;
using merge::Name;
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

/// A TArray<FName> as the apply reads it (data and count only).
struct LevelArray
{
    const Name*  data;
    std::int32_t num;
    std::int32_t max;
};

using IssueFn = char (*)(void* unused, const TraceArg* arg, std::uint64_t, std::uint64_t, std::uint64_t* handle);
using ReadFn  = char (*)(void* manager, const std::uint64_t* handle, std::uint8_t* block);
using ApplyFn = char (*)(void* manager, void* actors, const LevelArray* levels);
using ResetFn = void (*)(std::uint8_t* block);
using GrowFn  = void (*)(std::uint8_t* actors, std::int32_t oldNum);

constexpr int kMaxMergedLevels = 256;  // the game's level request holds up to 256 names

platform::CallRedirect g_issueCall, g_readCall, g_applyCall;
std::uintptr_t         g_issueSite = 0, g_issueTarget = 0;
IssueFn                g_issue   = nullptr;
ReadFn                 g_read    = nullptr;
ApplyFn                g_apply   = nullptr;
ResetFn                g_reset   = nullptr;
GrowFn                 g_grow    = nullptr;
std::uint8_t**         g_save    = nullptr;  // &the save data singleton
std::uint8_t**         g_manager = nullptr;  // &the manager (the reading)

// Game thread only: Tick, and the three calls inside the manager's evaluation.
bool                   g_enabled   = true;
bool                   g_placed    = false;
void*                  g_two       = nullptr;  // player 2's character while in
std::uint64_t          g_twoHandle = 0;        // player 2's trace, readable the frame after
alignas(16) std::array<std::uint8_t, facts::kEnvSetBlockBytes> g_twoBlock{};  // player 2's set (zero = constructed)
merge::TwoSet          g_twoSet;               // player 2's set as kept between reads
merge::TwoSet          g_fresh;
std::array<std::uint64_t, 3> g_twoIds{};       // player 2's set ids (the reading and the log)
merge::EntryList       g_added;                // the entries added to player 1's list at this frame's read
bool                   g_addedThisFrame = false;
merge::EntryList       g_applied;              // the entries the game last applied for player 2
merge::LevelList       g_appliedLevels;        // and the levels of the set they came from
merge::List<Name, kMaxMergedLevels> g_levels;  // the level list handed to the apply

std::uint64_t g_traces = 0, g_reads = 0, g_forced = 0, g_levelsAdded = 0;
std::wstring  g_problem;                       // the last thing that stopped player 2's part (logged on change)

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
    std::int64_t bits = 0;
    std::memcpy(&bits, &name, sizeof(bits));
    std::wstring text;
    return platform::CallGuarded(&NameText, bits, &text) && !text.empty() ? text : L"?";
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

std::wstring EntriesText(const merge::EntryList& list)
{
    std::wstring out;
    for (const Entry& entry : list.View())
    {
        out += std::format(L"{}{} ({})", out.empty() ? L"" : L", ", Text(entry.name), entry.hidden != 0 ? L"hidden" : L"shown");
    }
    return out.empty() ? L"none" : out;
}

void Problem(const wchar_t* what) noexcept
try
{
    if (g_problem != what)
    {
        g_problem = what;
        if (*what != L'\0')
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

/// The game's own append (EnvSetMgr_BuildSetLists_FromTables): raise the count, grow past the max, write the entry.
bool Append(std::uint8_t* block, const Entry& entry) noexcept
{
    std::int32_t num = 0, max = 0;
    if (!Read(block + facts::kEnvSetBlockActorsNum, num) || !Read(block + facts::kEnvSetBlockActorsMax, max))
    {
        return false;
    }
    const std::int32_t raised = num + 1;
    std::memcpy(block + facts::kEnvSetBlockActorsNum, &raised, sizeof(raised));
    if (raised > max)
    {
        g_grow(block + facts::kEnvSetBlockActors, num);
    }
    Entry*       data  = nullptr;
    std::int32_t count = 0;
    if (!Entries(block, data, count) || count != raised)
    {
        return false;
    }
    data[num] = entry;
    return true;
}

// --- 1. The trace --------------------------------------------------------------------------------------------------

char Issue(void* unused, const TraceArg* arg, std::uint64_t r8, std::uint64_t r9, std::uint64_t* handle) noexcept
{
    const char result = g_issue(unused, arg, r8, r9, handle);  // player 1's, as the game issues it
    if (!g_enabled || g_two == nullptr)
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

/// The game's read of player 2's trace into the mod's block, with the save data's set name kept player 1's.
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
    g_fresh = {};
    bool fits = true;
    for (std::int32_t i = 0; i < entryCount && fits; ++i)
    {
        fits = g_fresh.entries.Add(entries[i]);
    }
    for (std::int32_t i = 0; i < levelCount && fits; ++i)
    {
        fits = g_fresh.levels.Add(levels[i]);
    }
    if (!fits)
    {
        Problem(L"player 2's set lists more than the mod holds; kept the last one");
        return false;
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

char ReadSet(void* manager, const std::uint64_t* handle, std::uint8_t* block) noexcept
{
    const char one    = g_read(manager, handle, block);  // player 1's, as the game reads it
    g_addedThisFrame  = false;
    std::uint8_t hold = 0;
    if (one == 0 || !Read(static_cast<std::uint8_t*>(manager) + facts::kEnvSetMgrHoldMode, hold) || hold != 0)
    {
        return one;  // no set built (the game changes nothing), or hold mode (the game replaces the block)
    }
    const bool present  = g_enabled && g_two != nullptr;
    const bool resolved = present && ReadTwo(manager);
    if (!present && g_twoIds != std::array<std::uint64_t, 3>{})
    {
        g_twoIds = {};
    }
    g_twoSet = merge::NextTwoSet(present, resolved, g_fresh, g_twoSet);

    Entry*       mine = nullptr;
    std::int32_t num  = 0;
    if (!Entries(block, mine, num))
    {
        return one;
    }
    merge::EntryList wanted;
    if (!merge::Extras({mine, static_cast<std::size_t>(num)}, g_twoSet.entries.View(), wanted))
    {
        Problem(L"too many of player 2's actors to add; none added");
        wanted.count = 0;
    }
    g_added = {};
    for (const Entry& entry : wanted.View())
    {
        if (!Append(block, entry))
        {
            Problem(L"an actor of player 2's set could not be added to the game's list");
            break;
        }
        g_added.Add(entry);
    }
    g_addedThisFrame = true;
    if (merge::MustApply(g_added, g_applied))
    {
        // The game's change test sees a new set id and runs its own change path: copy, apply, level request.
        constexpr std::uint64_t kNoId = 0;
        if (platform::WriteGuarded(static_cast<std::uint8_t*>(manager) + facts::kEnvSetMgrSetIds, &kNoId, sizeof(kNoId)))
        {
            ++g_forced;
            platform::LogDebug(L"env sets 2: the game applies its set again; player 2's ground adds {}", EntriesText(g_added));
        }
    }
    return one;
}

// --- 3. The apply --------------------------------------------------------------------------------------------------

char Apply(void* manager, void* actors, const LevelArray* levels) noexcept
{
    if (g_addedThisFrame)
    {
        g_applied       = g_added;
        g_appliedLevels = g_twoSet.levels;
        g_addedThisFrame = false;
    }
    // Hold mode applies the manager's last list, which still holds the entries last added: their levels stay handed.
    std::int32_t num  = 0;
    const Name*  data = nullptr;
    if (g_applied.count == 0 || g_appliedLevels.count == 0 || levels == nullptr || !Read(&levels->num, num) ||
        !Read(&levels->data, data) || num < 0 || num > kMaxMergedLevels || (num > 0 && data == nullptr))
    {
        return g_apply(manager, actors, levels);
    }
    g_levels.count = 0;
    bool fits = true;
    for (std::int32_t i = 0; i < num && fits; ++i)
    {
        Name level;
        fits = Read(data + i, level) && g_levels.Add(level);
    }
    const int before = g_levels.count;
    fits = fits && merge::AddMissingLevels(std::span<const Name>{}, g_appliedLevels.View(), g_levels);  // skips listed
    if (!fits)
    {
        Problem(L"the levels to search do not fit; the game's own levels used");
        return g_apply(manager, actors, levels);
    }
    if (g_levels.count == before)
    {
        return g_apply(manager, actors, levels);  // player 2's actors are in player 1's levels
    }
    g_levelsAdded += static_cast<std::uint64_t>(g_levels.count - before);
    const LevelArray merged{g_levels.items.data(), g_levels.count, g_levels.count};
    return g_apply(manager, actors, &merged);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto issueCall = image.Rebase(facts::kEnvSetIssueTraceCall, 5);
    const auto readCall  = image.Rebase(facts::kEnvSetReadResultCall, 5);
    const auto applyCall = image.Rebase(facts::kEnvSetApplyCall, 5);
    const auto issue     = platform::FindFunction(image, facts::kEnvSetIssueTrace);
    const auto read      = platform::FindFunction(image, facts::kEnvSetReadResult);
    const auto apply     = platform::FindFunction(image, facts::kEnvSetApply);
    const auto reset     = platform::FindFunction(image, facts::kEnvSetBlockReset);
    const auto grow      = platform::FindFunction(image, facts::kEnvSetActorsGrow);
    const auto save      = image.Rebase(facts::kSaveDataGlobal, sizeof(void*));
    const auto manager   = image.Rebase(facts::kEnvSetManagerGlobal, sizeof(void*));
    if (!issueCall || !readCall || !applyCall || !issue || !read || !apply || !reset || !grow || !save || !manager)
    {
        platform::LogWarning(L"env sets 2: the EnvironmentSet code is not what the research read; only player 1's ground "
                             L"hides the area's actors");
        return false;
    }
    g_issue   = reinterpret_cast<IssueFn>(*issue);
    g_read    = reinterpret_cast<ReadFn>(*read);
    g_apply   = reinterpret_cast<ApplyFn>(*apply);
    g_reset   = reinterpret_cast<ResetFn>(*reset);
    g_grow    = reinterpret_cast<GrowFn>(*grow);
    g_save    = reinterpret_cast<std::uint8_t**>(*save);
    g_manager = reinterpret_cast<std::uint8_t**>(*manager);
    // The apply first: a read that adds entries must never reach an apply that cannot find them.
    if (!g_applyCall.Install(*applyCall, *apply, reinterpret_cast<std::uintptr_t>(&Apply)) ||
        !g_readCall.Install(*readCall, *read, reinterpret_cast<std::uintptr_t>(&ReadSet)))
    {
        g_readCall.Restore();
        g_applyCall.Restore();
        platform::LogWarning(L"env sets 2: the read or the apply could not be redirected; only player 1's ground hides "
                             L"the area's actors");
        return false;
    }
    g_issueSite   = *issueCall;
    g_issueTarget = *issue;
    control::Register({.name  = L"env-sets",
                       .shape = control::Shape::OnOff,
                       .help  = L"the ground under player 2 hides what the area hides, as player 1's does "
                                L"(world/env_sets_two.h; on)",
                       .set   = [](const control::Value& value) {
                           SetEnabled(std::get<bool>(value));
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled}; }});
    control::Register({.name = L"world.env-sets", .shape = control::Shape::Text,
                       .help = L"player 2's EnvironmentSet, the actors it adds to player 1's, the game's re-applies",
                       .get  = [] { return control::Value{Reading()}; }});
    platform::Log(L"env sets 2: read and apply redirected; player 2's trace is placed on the first tick");
    return true;
}

void Tick(void* memberTwo) noexcept
{
    g_two = memberTwo;
    if (g_placed || g_issueSite == 0)
    {
        return;
    }
    g_placed = true;  // once, whatever the outcome; outside the world tick, as the crossing call needs
    if (g_issueCall.InstallFromOwnThread(g_issueSite, g_issueTarget, reinterpret_cast<std::uintptr_t>(&Issue)))
    {
        platform::Log(L"env sets 2: the trace is redirected; the ground under player 2 hides what the area hides");
    }
    else
    {
        platform::LogWarning(L"env sets 2: the trace could not be redirected; only player 1's ground hides the area's actors");
    }
}

void SetEnabled(bool on) noexcept
{
    g_enabled = on;
    platform::Log(L"env sets 2 {}", on ? L"on" : L"off (player 2's actors are dropped at the game's next read)");
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
    return std::format(L"{}; trace {}; applied set {}; player 2's set {}; added for player 2: {}; player 2's traces {}, "
                       L"reads {}, the game re-applied {} times, levels handed to the apply {}{}",
                       g_enabled ? L"on" : L"off", g_issueCall.IsInstalled() ? L"placed" : L"not placed", IdsText(oneIds),
                       IdsText(g_twoIds), EntriesText(g_applied), g_traces, g_reads, g_forced, g_levelsAdded,
                       g_problem.empty() ? std::wstring{} : L"; last problem: " + g_problem);
}
catch (...)
{
    return L"?";
}

}  // namespace lss::world::env_sets_two
