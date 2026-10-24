#include "render/fog_volume_watch.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <Unreal/NameTypes.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace lss::render::fog_volume_watch {

namespace {

using ComputeFn = std::uintptr_t (*)(void* renderer, void* commandList);  // whatever the game returns passes through

constexpr int         kMostViews   = 4;
constexpr std::size_t kMostVolumes = 256;  // a list longer than this is read up to here
constexpr std::size_t kMostNamed   = 8;    // names in one line

// Set once at install, on the game thread, before the redirects are live; read on the render thread afterwards.
std::array<platform::CallRedirect, facts::kComputeVolumetricFogCalls.size()> g_calls;
ComputeFn                                                                    g_compute = nullptr;
std::atomic<bool>                                                            g_installed{false};

/// One view's record over the current one-second window. Render thread only.
struct ViewRecord
{
    std::vector<const void*> drawn;  // last frame's entries the fog takes, sorted
    std::vector<const void*> came;   // entries that joined this window
    std::vector<const void*> went;   // entries that left this window
    int                      listedLow  = -1;
    int                      listedHigh = -1;
    int                      drawnLow   = -1;
    int                      drawnHigh  = -1;
    unsigned                 changes    = 0;
};

std::array<ViewRecord, kMostViews>    g_views;
int                                   g_watchedCount = 0;  // the view count the records belong to
std::chrono::steady_clock::time_point g_windowStart{};
std::wstring                          g_lastSummary;  // render thread: the last summary line written

std::mutex   g_readingLock;  // guards g_reading (written on the render thread, read on the game thread)
std::wstring g_reading = L"-";

template <class T>
bool ReadAt(const void* base, std::size_t offset, T& out) noexcept
{
    return base != nullptr && platform::CopyGuarded(&out, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
}

/// The name's text through UE4SS's FName (the game's own name pool), or empty. A free function for the guard.
void NameText(std::int64_t bits, std::wstring* out) noexcept
try
{
    *out = RC::Unreal::FName(bits).ToString();
}
catch (...)
{
    out->clear();
}

/// "owner (resource)" of a scene proxy, for the log.
std::wstring Describe(const void* proxy)
{
    std::int64_t owner = 0;
    std::int64_t res   = 0;
    std::wstring ownerText;
    std::wstring resText;
    if (ReadAt(proxy, facts::kProxyOwnerName, owner))
    {
        platform::CallGuarded(&NameText, owner, &ownerText);
    }
    if (ReadAt(proxy, facts::kProxyResourceName, res))
    {
        platform::CallGuarded(&NameText, res, &resText);
    }
    return (ownerText.empty() ? L"?" : ownerText) + L" (" + (resText.empty() ? L"?" : resText) + L")";
}

std::wstring Names(const std::vector<const void*>& proxies)
{
    std::wstring text;
    for (std::size_t i = 0; i < proxies.size() && i < kMostNamed; ++i)
    {
        text += (i == 0 ? L"" : L", ") + Describe(proxies[i]);
    }
    if (proxies.size() > kMostNamed)
    {
        text += std::format(L", +{}", proxies.size() - kMostNamed);
    }
    return text;
}

/// Whether the view's fog takes the entry: its bit in the view's visibility map (the voxelization's own test).
bool Drawn(const std::uint8_t* view, const void* proxy) noexcept
{
    const void*  info  = nullptr;
    std::int32_t index = -1;
    if (!ReadAt(proxy, facts::kProxySceneInfo, info) || !ReadAt(info, facts::kSceneInfoPackedIndex, index) || index < 0)
    {
        return false;
    }
    const std::uint32_t* heap = nullptr;
    if (!ReadAt(view, facts::kViewVisibilityHeap, heap))
    {
        return false;
    }
    const auto*   words = heap != nullptr ? heap : reinterpret_cast<const std::uint32_t*>(view + facts::kViewVisibilityInline);
    std::uint32_t word  = 0;
    return platform::CopyGuarded(&word, words + (index >> 5), sizeof(word)) && ((word >> (index & 31)) & 1u) != 0;
}

void Widen(int& low, int& high, int value) noexcept
{
    low  = low < 0 ? value : std::min(low, value);
    high = std::max(high, value);
}

/// Adds the entries of `from` missing in `to` (both sorted) to `into`, once each.
void Missing(const std::vector<const void*>& from, const std::vector<const void*>& to, std::vector<const void*>& into)
{
    for (const void* p : from)
    {
        if (!std::binary_search(to.begin(), to.end(), p) && std::find(into.begin(), into.end(), p) == into.end())
        {
            into.push_back(p);
        }
    }
}

/// The views' lists this frame; once a second, the summary. Render thread. Only a draw of two or more views is
/// watched (the split screen's or the mirror's): scene and reflection captures run the same call with one view of
/// their own, and would mix into view 1's record.
void Watch(void* renderer)
{
    const std::uint8_t* views = nullptr;
    std::int32_t        count = 0;
    if (!ReadAt(renderer, facts::kRendererViews, views) || !ReadAt(renderer, facts::kRendererViewCount, count) ||
        views == nullptr || count < 2)
    {
        return;
    }
    count = std::min(count, kMostViews);
    if (count != g_watchedCount)
    {
        g_views         = {};  // a view joined or left: start the records again
        g_watchedCount = count;
    }
    std::vector<const void*> listed;
    std::vector<const void*> drawn;
    for (int v = 0; v < count; ++v)
    {
        const std::uint8_t* view = views + static_cast<std::size_t>(v) * facts::kViewInfoStride;
        const void* const*  data = nullptr;
        std::int32_t        n    = 0;
        if (!ReadAt(view, facts::kViewFogVolumes, data) || !ReadAt(view, facts::kViewFogVolumeCount, n) || n < 0)
        {
            continue;
        }
        listed.assign(std::min<std::size_t>(static_cast<std::size_t>(n), kMostVolumes), nullptr);
        if (!listed.empty() && !platform::CopyGuarded(listed.data(), data, listed.size() * sizeof(void*)))
        {
            continue;
        }
        drawn.clear();
        for (const void* proxy : listed)
        {
            if (proxy != nullptr && Drawn(view, proxy))
            {
                drawn.push_back(proxy);
            }
        }
        std::sort(drawn.begin(), drawn.end());

        ViewRecord& record = g_views[static_cast<std::size_t>(v)];
        Widen(record.listedLow, record.listedHigh, n);
        Widen(record.drawnLow, record.drawnHigh, static_cast<int>(drawn.size()));
        if (drawn != record.drawn)
        {
            ++record.changes;
            std::vector<const void*> came;
            std::vector<const void*> went;
            Missing(drawn, record.drawn, came);
            Missing(record.drawn, drawn, went);
            Missing(came, {}, record.came);
            Missing(went, {}, record.went);
            if (platform::Enabled(platform::Level::Trace))
            {
                platform::LogTrace(L"fog volumes: view {} takes {} of {} (came: {}; went: {})", v + 1, drawn.size(), n,
                                   came.empty() ? L"-" : Names(came), went.empty() ? L"-" : Names(went));
            }
            record.drawn.swap(drawn);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - g_windowStart < std::chrono::seconds(1))
    {
        return;
    }
    g_windowStart = now;
    std::wstring summary;
    std::wstring details;
    bool         changed = false;
    for (int v = 0; v < count; ++v)
    {
        ViewRecord& record = g_views[static_cast<std::size_t>(v)];
        summary += std::format(L"{}view {}: {}..{} listed, {}..{} taken, {} changes", v == 0 ? L"" : L" | ", v + 1,
                               record.listedLow, record.listedHigh, record.drawnLow, record.drawnHigh, record.changes);
        if (record.changes != 0)
        {
            changed = true;
        }
        if (record.changes != 0 && platform::Enabled(platform::Level::Debug))
        {
            details += std::format(L"; view {} came: {}; went: {}", v + 1,
                                   record.came.empty() ? L"-" : Names(record.came),
                                   record.went.empty() ? L"-" : Names(record.went));
        }
    }
    for (ViewRecord& record : g_views)
    {
        record.came.clear();
        record.went.clear();
        record.listedLow = record.listedHigh = record.drawnLow = record.drawnHigh = -1;
        record.changes                                                       = 0;
    }
    if (changed || summary != g_lastSummary)
    {
        platform::LogDebug(L"fog volumes: {}{}", summary, details);
    }
    g_lastSummary = summary;
    const std::lock_guard lock(g_readingLock);
    g_reading = summary;
}

void WatchGuarded(void* renderer) noexcept
try
{
    Watch(renderer);
}
catch (...)
{
}

/// The game's call, after the watch reads the views' lists (which are final by now).
std::uintptr_t Compute(void* renderer, void* commandList)
{
    WatchGuarded(renderer);
    return g_compute(renderer, commandList);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto target = platform::FindFunction(image, facts::kComputeVolumetricFog);
    if (!target)
    {
        platform::LogWarning(L"fog volume watch: an engine check failed; the watch is not available");
        return false;
    }
    g_compute = reinterpret_cast<ComputeFn>(*target);
    for (std::size_t i = 0; i < g_calls.size(); ++i)
    {
        const auto site = image.Rebase(facts::kComputeVolumetricFogCalls[i], 5);
        if (!site || !g_calls[i].Install(*site, *target, reinterpret_cast<std::uintptr_t>(&Compute)))
        {
            platform::LogWarning(L"fog volume watch: call {} is not the call the research read; the watch is not available",
                                 i + 1);
            for (auto& call : g_calls)
            {
                call.Restore();
            }
            return false;
        }
    }
    g_installed.store(true);
    control::Register({.name  = L"fog-volumes",
                       .shape = control::Shape::Text,
                       .help  = L"each view's fog volumes over the last second: listed, taken by the fog, changes "
                                L"(render/fog_volume_watch.h)",
                       .get   = [] {
                           std::wstring text;
                           try
                           {
                               const std::lock_guard lock(g_readingLock);
                               text = g_reading;
                           }
                           catch (...)
                           {
                           }
                           return control::Value{text};
                       }});
    platform::Log(L"fog volume watch: installed (reading fog-volumes; changes at log level debug)");
    return true;
}

void Remove() noexcept
{
    if (!g_installed.exchange(false))
    {
        return;
    }
    for (auto& call : g_calls)
    {
        if (!call.Restore())
        {
            platform::LogWarning(L"fog volume watch: a call was changed after the watch; it is left as found");
        }
    }
}

}  // namespace lss::render::fog_volume_watch
