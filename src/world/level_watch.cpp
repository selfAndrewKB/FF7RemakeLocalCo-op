#include "world/level_watch.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/name_text.h"
#include "platform/verified_code.h"
#include "players/game_state.h"
#include "players/seats.h"
#include "world/env_sets.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>

namespace lss::world::level_watch {

namespace {

using platform::Read;

using SetSlotFn = void (*)(void* holder, std::int32_t slot, std::uint64_t id);
using EnqueueFn = std::int64_t (*)(std::int32_t priority, std::uint64_t level, bool load, bool hide, bool flag);

constexpr std::int32_t kMaxLevels   = 2048;  // a world's streaming levels read at most (Ch. 8 lists 460)
constexpr auto         kMemoryEvery = std::chrono::seconds(10);

platform::CallRedirect  g_slotCall;
platform::EntryRedirect g_enqueue;

std::atomic<bool>          g_on{true};
std::atomic<std::uint32_t> g_loadRequests{0};
std::atomic<std::uint32_t> g_unloadRequests{0};

/// One streaming level as the last census saw it, keyed by its streaming object (an entry added or removed in the middle
/// of the world's list moves the others' positions, not their objects). Game thread only.
struct Level
{
    std::wstring  name;
    std::uint32_t wanted = 0;  // the flags' should-load and should-show bits
    bool          loaded = false;
    std::uint32_t pass   = 0;  // the census that last saw it
};
std::unordered_map<void*, Level> g_levels;
void*                            g_world   = nullptr;  // null while the census rests (no further player in)
std::uint32_t                    g_pass    = 0;
bool                             g_capped  = false;    // the world lists more than kMaxLevels
std::wstring                     g_lastArea = L"none seen";  // player 1's last area set (slot 32), as logged
std::chrono::steady_clock::time_point g_lastMemory{};
std::uint64_t                    g_censusTicks  = 0;  // the census's own time since the last 10 s line (QPC ticks)
std::uint32_t                    g_censusPasses = 0;

/// The name's text; "?" when the engine cannot say.
std::wstring NameText(std::uint64_t bits)
{
    std::wstring text = platform::NameText(bits);
    return text.empty() ? L"?" : text;
}

/// A streaming level's name as the game's queue lines give it. Its PackageName, else its PackageNameToLoad, else the
/// asset name of its WorldAsset's path ("/Game/…/030-SLUM7_Layout_045.030-SLUM7_Layout_045": the part after the last
/// dot, or after the last slash). This build leaves PackageName empty (2026-10-29: 563 census lines named None).
std::wstring LevelName(const void* streaming) noexcept
try
{
    const auto*   base = static_cast<const std::uint8_t*>(streaming);
    std::uint64_t bits = 0;
    for (const std::uint32_t offset : {facts::kLevelStreamingPackage, facts::kLevelStreamingPackageToLoad})
    {
        if (Read(base + offset, bits) && static_cast<std::uint32_t>(bits) != 0)  // the comparison index: 0 is None
        {
            return NameText(bits);
        }
    }
    if (!Read(base + facts::kLevelStreamingWorldAssetPath, bits) || static_cast<std::uint32_t>(bits) == 0)
    {
        return L"None";
    }
    std::wstring path = NameText(bits);
    if (const auto dot = path.rfind(L'.'); dot != std::wstring::npos && dot + 1 < path.size())
    {
        return path.substr(dot + 1);
    }
    if (const auto slash = path.rfind(L'/'); slash != std::wstring::npos && slash + 1 < path.size())
    {
        return path.substr(slash + 1);
    }
    return path;
}
catch (...)
{
    return L"?";
}

std::uint64_t CommittedMegabytes() noexcept
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    return K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != 0
               ? counters.PrivateUsage / (1024 * 1024)
               : 0;
}

struct Counts
{
    int listed = 0, wantedLoaded = 0, loaded = 0, wantedShown = 0;
};

Counts Count() noexcept
{
    Counts counts;
    for (const auto& [streaming, level] : g_levels)
    {
        ++counts.listed;
        counts.wantedLoaded += (level.wanted & facts::kLevelStreamingShouldLoad) != 0 ? 1 : 0;
        counts.wantedShown += (level.wanted & facts::kLevelStreamingShouldShow) != 0 ? 1 : 0;
        counts.loaded += level.loaded ? 1 : 0;
    }
    return counts;
}

std::wstring CountsText(const Counts& counts)
{
    return std::format(L"{} listed, {} wanted loaded, {} loaded, {} wanted shown", counts.listed, counts.wantedLoaded, counts.loaded,
                       counts.wantedShown);
}

std::wstring SeatsText()
{
    std::wstring text;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        text += std::format(L"{}{}'s ground {}", seat == 1 ? L"" : L"; ", players::SeatName(seat), env_sets::SeatSetText(seat));
    }
    return text;
}

/// Player 1's area change, the residency half: slot 32 takes the area's planner level set (the loader adds it, then
/// releases the old one). Runs inside the EnvironmentSet manager's tick, on the game thread (the call env_sets traces
/// from), so env_sets' per-seat state may be read here.
void SetSlot(void* holder, std::int32_t slot, std::uint64_t id)
{
    const auto original = reinterpret_cast<SetSlotFn>(g_slotCall.Original());
    if (g_on.load(std::memory_order_relaxed) && slot >= 0 && slot < facts::kPlannerSlots)
    {
        try
        {
            std::uint64_t old = 0;
            Read(static_cast<const std::uint8_t*>(holder) + facts::kPlannerSlotIds + 8 * static_cast<std::size_t>(slot), old);
            g_lastArea = NameText(id);
            platform::Log(L"level watch: player 1's area takes planner level set {} in residency slot {} (was {}); {}; levels: {}, "
                          L"{} MB committed",
                          g_lastArea, slot, NameText(old), SeatsText(),
                          g_world != nullptr ? CountsText(Count()) : std::wstring(L"not counted (no further player in)"), CommittedMegabytes());
        }
        catch (...)
        {
        }
    }
    original(holder, slot, id);
}

/// Every request into the game's level queue (a load's first reference, an unload's last release, a script's own).
std::int64_t Enqueue(std::int32_t priority, std::uint64_t level, bool load, bool hide, bool flag)
{
    const auto original = reinterpret_cast<EnqueueFn>(g_enqueue.Original());
    if (g_on.load(std::memory_order_relaxed))
    {
        (load ? g_loadRequests : g_unloadRequests).fetch_add(1, std::memory_order_relaxed);
        if (platform::Enabled(platform::Level::Debug))
        {
            platform::LogDebug(L"level watch: the game queues the {} of {} (priority {}, hide {}, flag {})", load ? L"load" : L"unload",
                               NameText(level), priority, hide ? 1 : 0, flag ? 1 : 0);
        }
    }
    return original(priority, level, load, hide, flag);
}

/// One streaming level's state now; false if it could not be read.
bool ReadLevel(void* streaming, std::uint32_t& wanted, bool& loaded) noexcept
{
    std::uint32_t flags      = 0;
    void*         levelObject = nullptr;
    if (streaming == nullptr || !Read(static_cast<const std::uint8_t*>(streaming) + facts::kLevelStreamingFlags, flags) ||
        !Read(static_cast<const std::uint8_t*>(streaming) + facts::kLevelStreamingLoaded, levelObject))
    {
        return false;
    }
    wanted = flags & (facts::kLevelStreamingShouldLoad | facts::kLevelStreamingShouldShow);
    loaded = levelObject != nullptr;
    return true;
}

std::wstring StateText(std::uint32_t wanted, bool loaded)
{
    return std::format(L"wanted loaded {}, loaded {}, wanted shown {}", (wanted & facts::kLevelStreamingShouldLoad) != 0 ? L"yes" : L"no",
                       loaded ? L"yes" : L"no", (wanted & facts::kLevelStreamingShouldShow) != 0 ? L"yes" : L"no");
}

/// Compares the world's streaming levels with the last census: a level whose wanted bits or level object changed is one
/// Debug line, a level new to the list or gone from it one line each; a new world (or the census resuming) one Info line
/// with its counts, and no per-level lines.
void Census() noexcept
try
{
    void* const world = players::game_state::World();
    if (world == nullptr)
    {
        return;
    }
    void*        data  = nullptr;
    std::int32_t count = 0;
    if (!Read(static_cast<const std::uint8_t*>(world) + facts::kWorldStreamingLevels, data) ||
        !Read(static_cast<const std::uint8_t*>(world) + facts::kWorldStreamingLevels + 8, count) || count < 0 || (count > 0 && data == nullptr))
    {
        return;
    }
    g_capped = count > kMaxLevels;
    count    = std::min(count, kMaxLevels);
    const bool fresh = world != g_world;
    if (fresh)
    {
        g_world = world;
        g_levels.clear();
    }
    ++g_pass;
    for (std::int32_t i = 0; i < count; ++i)
    {
        void*         streaming = nullptr;
        std::uint32_t wanted    = 0;
        bool          loaded    = false;
        if (!Read(static_cast<void* const*>(data) + i, streaming))
        {
            if (fresh)
            {
                g_world = nullptr;  // a new world's first pass is whole or it starts over (its Info line included)
            }
            return;  // a partial pass: nothing is taken as gone; the next tick catches up
        }
        const auto found = g_levels.find(streaming);
        if (!ReadLevel(streaming, wanted, loaded))
        {
            if (found != g_levels.end())
            {
                found->second.pass = g_pass;  // unreadable this tick, still listed: kept as it was
            }
            continue;
        }
        if (found == g_levels.end())
        {
            Level level{LevelName(streaming), wanted, loaded, g_pass};
            if (!fresh)
            {
                platform::LogDebug(L"level watch: level {} listed: {}", level.name, StateText(wanted, loaded));
            }
            g_levels.emplace(streaming, std::move(level));
            continue;
        }
        Level& level = found->second;
        level.pass   = g_pass;
        if (level.wanted != wanted || level.loaded != loaded)
        {
            platform::LogDebug(L"level watch: level {}: {} (was {})", level.name, StateText(wanted, loaded), StateText(level.wanted, level.loaded));
            level.wanted = wanted;
            level.loaded = loaded;
        }
    }
    for (auto it = g_levels.begin(); it != g_levels.end();)
    {
        if (it->second.pass != g_pass)
        {
            platform::LogDebug(L"level watch: level {} no longer listed", it->second.name);
            it = g_levels.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (fresh)
    {
        platform::Log(L"level watch: a world with {}{}; {} MB committed", CountsText(Count()),
                      g_capped ? std::format(L" (the first {} of a longer list)", kMaxLevels) : std::wstring{}, CommittedMegabytes());
    }
}
catch (...)
{
}

std::wstring ReadingText()
{
    return std::format(L"{}; levels{}: {}{}; player 1's last area set {}; {}; requests seen: {} loads, {} unloads; {} MB committed",
                       g_on.load(std::memory_order_relaxed) ? L"on" : L"off", g_world == nullptr ? L" (resting: no further player in)" : L"",
                       CountsText(Count()), g_capped ? std::format(L" (the first {} only)", kMaxLevels) : std::wstring{}, g_lastArea, SeatsText(),
                       g_loadRequests.load(std::memory_order_relaxed), g_unloadRequests.load(std::memory_order_relaxed),
                       CommittedMegabytes());
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_slotCall.IsInstalled() || g_enqueue.IsInstalled())
    {
        return false;
    }
    const auto slotTarget = platform::FindFunction(image, facts::kPlannerSetSlot);
    const auto enqueue    = platform::FindFunction(image, facts::kLevelRequestEnqueue);
    const auto slotSite   = image.Rebase(facts::kEnvSetPlannerSlotCall, 5);
    if (!slotTarget || !enqueue || !slotSite)
    {
        platform::LogWarning(L"level watch: the level code is not what the research read; the world's levels are not watched");
        return false;
    }
    if (!g_slotCall.Install(*slotSite, *slotTarget, reinterpret_cast<std::uintptr_t>(&SetSlot)) ||
        !g_enqueue.Install(*enqueue, facts::kLevelRequestEnqueueFirst, reinterpret_cast<std::uintptr_t>(&Enqueue)))
    {
        g_slotCall.Restore();
        g_enqueue.Restore();
        platform::LogWarning(L"level watch: the area's residency call or the level queue could not be redirected; the world's levels are not watched");
        return false;
    }
    control::Register({.name  = L"level-watch",
                       .shape = control::Shape::OnOff,
                       .help  = L"logs what loads, shows, hides and unloads the world's levels (world/level_watch.h; on)",
                       .set   = [](const control::Value& value) {
                           g_on.store(std::get<bool>(value), std::memory_order_relaxed);
                           platform::Log(L"level watch {}", std::get<bool>(value) ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_on.load(std::memory_order_relaxed)}; }});
    control::Register({.name  = L"world.levels",
                       .shape = control::Shape::Text,
                       .help  = L"the world's streaming levels counted, player 1's last area set, each further player's ground set, "
                                L"the level requests seen, the committed memory",
                       .get   = [] { return control::Value{ReadingText()}; }});
    platform::Log(L"level watch: ready (the area's residency call and the level queue are watched; world/area_levels logs the "
                  L"area's level request)");
    return true;
}

void Tick(bool anyIn) noexcept
{
    if (!g_on.load(std::memory_order_relaxed) || !anyIn)
    {
        // Single player costs nothing: the census rests, and its first look after a join is a fresh world's Info line.
        g_world = nullptr;
        g_levels.clear();
        return;
    }
    LARGE_INTEGER start{}, end{};
    QueryPerformanceCounter(&start);
    Census();
    QueryPerformanceCounter(&end);
    g_censusTicks += static_cast<std::uint64_t>(end.QuadPart - start.QuadPart);
    ++g_censusPasses;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastMemory >= kMemoryEvery)
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const double microseconds = g_censusPasses != 0 && frequency.QuadPart != 0
                                        ? 1e6 * static_cast<double>(g_censusTicks) / static_cast<double>(frequency.QuadPart) / g_censusPasses
                                        : 0.0;
        g_lastMemory   = now;
        g_censusTicks  = 0;
        g_censusPasses = 0;
        platform::LogDebug(L"level watch (10 s): levels: {}; {} MB committed; {}; the census took {:.1f} us per tick", CountsText(Count()),
                           CommittedMegabytes(), SeatsText(), microseconds);
    }
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_relaxed);
    g_slotCall.Restore();
    g_enqueue.Restore();
}

std::wstring Reading() noexcept
try
{
    return ReadingText();
}
catch (...)
{
    return L"?";
}

}  // namespace lss::world::level_watch
