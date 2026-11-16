#include "render/frame_watch.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/hitch_step.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/perf_counter.h"
#include "platform/verified_code.h"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace lss::render::frame_watch {

namespace {

constexpr double       kAverageWeight = 0.05;  // the running average: each frame moves it 5 % of the way
constexpr double       kHitchRatio    = 1.6;   // a hitch is this much over the average...
constexpr double       kHitchMarginMs = 5.0;   // ...and at least this many ms over it
constexpr unsigned     kNewNormalRun  = 30;    // this many hitch frames in a row become the new average
constexpr std::int64_t kWindowSeconds = 10;
constexpr std::int64_t kShortestWindow = 1;

/// The engine's numbers, in the order of the facts' sites.
enum Engine : std::size_t
{
    Game,
    Render,
    Present,
    Gpu,
    kEngineCount,
};
constexpr std::array<const wchar_t*, kEngineCount> kEngineNames{L"game", L"render", L"present", L"GPU"};
constexpr std::array<std::uintptr_t, kEngineCount> kEngineGlobals{facts::kGameThreadTime, facts::kRenderThreadTime,
                                                                  facts::kSwapBufferTime, facts::kGpuFrameTime};

// The instructions that name the globals (research/RE-frame-stats.md): each a RIP-relative store or load whose
// displacement is the global's, so the bytes check the address.
constexpr std::array<std::uint8_t, 6> kGameThreadTimeStore{0x89, 0x0D, 0x83, 0xED, 0x51, 0x02};
constexpr std::array<std::uint8_t, 6> kRenderThreadTimeStore{0x89, 0x05, 0xBD, 0xDE, 0xFA, 0x02};
constexpr std::array<std::uint8_t, 6> kSwapBufferTimeStore{0x89, 0x0D, 0x01, 0xDF, 0xFA, 0x02};
constexpr std::array<std::uint8_t, 7> kGpuFrameTimeLoad{0x8B, 0x05, 0x6A, 0xD5, 0x8F, 0x03, 0xC3};
constexpr std::array<std::uint8_t, 7> kRenderingThreadStore{0x48, 0x89, 0x05, 0x62, 0xAB, 0x10, 0x03};
constexpr std::array<std::uint8_t, 7> kRhiThreadStore{0x48, 0x89, 0x0D, 0x35, 0xA5, 0x10, 0x03};
const std::array<platform::CodeSite, 4> kTimeSites{{
    {L"FViewport::Draw's store of the game thread's time", 0x14330D743, kGameThreadTimeStore},
    {L"the Slate window draw's store of the render thread's time", 0x14287E60D, kRenderThreadTimeStore},
    {L"the Slate window draw's store of the present's time", 0x14287E5B5, kSwapBufferTimeStore},
    {L"RHIGetGPUFrameCycles' load of the GPU's frame time", 0x141F33BD0, kGpuFrameTimeLoad},
}};
const std::array<platform::CodeSite, 2> kThreadSites{{
    {L"StartRenderingThread's store of the render thread", 0x1426F018F, kRenderingThreadStore},
    {L"StartRenderingThread's store of the RHI thread", 0x1426EFFA4, kRhiThreadStore},
}};

/// One engine number through a window: its sum and worst in ms, and whether it changed (a GPU number that never does is stale).
struct Number
{
    double        sumMs   = 0.0;
    double        worstMs = 0.0;
    double        lastMs  = 0.0;
    std::uint32_t lastRaw = 0;
    bool          changed = false;
};

/// A frame thread's CPU time as Windows counts it (GetThreadTimes, 100 ns units), from the window's start.
struct Thread
{
    std::uint32_t id     = 0;
    HANDLE        handle = nullptr;
    std::uint64_t start  = 0;  // its CPU time at the window's start; 0: not read yet
};
enum ThreadIndex : std::size_t
{
    GameThread,
    RenderThread,
    RhiThread,
    kThreadCount,
};

// Set at install, then only read.
bool                                             g_installed = false;
double                                           g_msPerTick = 0.0;
std::array<const std::uint32_t*, kEngineCount>   g_engine{};  // null: the group's check failed
const void* const*                               g_renderingThread = nullptr;
const void* const*                               g_rhiThread       = nullptr;

// Game thread.
std::int64_t                          g_lastTick    = 0;
std::int64_t                          g_windowStart = 0;
platform::HitchState                  g_hitch;
std::uint32_t                         g_frames  = 0;
std::uint32_t                         g_hitches = 0;
double                                g_frameSumMs   = 0.0;
double                                g_frameWorstMs = 0.0;
std::array<Number, kEngineCount>      g_numbers{};
std::array<Thread, kThreadCount>      g_threads{};
std::int64_t                          g_windowSeconds = kWindowSeconds;
std::wstring                          g_last          = L"no window yet";

/// The id of the thread a FRunnableThread* global names, or 0 (unset or unread).
std::uint32_t ThreadIdOf(const void* const* global) noexcept
{
    const void*   thread = nullptr;
    std::uint32_t id     = 0;
    return global != nullptr && platform::Read(global, thread) && thread != nullptr &&
                   platform::Read(static_cast<const std::uint8_t*>(thread) + facts::kRunnableThreadId, id)
               ? id
               : 0;
}

std::uint64_t CpuTime(HANDLE handle) noexcept
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (handle == nullptr || !GetThreadTimes(handle, &created, &exited, &kernel, &user))
    {
        return 0;
    }
    const auto value = [](const FILETIME& t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return value(kernel) + value(user);
}

/// Follows the thread's id (a restarted render thread is a new thread) and returns its CPU time per frame in ms over
/// the window, or a negative number when it was not followed for the whole window; starts the next window.
double EndThreadWindow(Thread& t, std::uint32_t id, std::uint32_t frames) noexcept
{
    if (id != t.id)
    {
        if (t.handle != nullptr)
        {
            CloseHandle(t.handle);
        }
        t.id     = id;
        t.handle = id != 0 ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id) : nullptr;
        t.start  = CpuTime(t.handle);
        return -1.0;
    }
    const std::uint64_t now    = CpuTime(t.handle);
    const double        perFrame = t.start != 0 && now >= t.start && frames != 0
                                       ? static_cast<double>(now - t.start) / 10000.0 / frames
                                       : -1.0;
    t.start = now;
    return perFrame;
}

std::wstring Ms(double ms)
{
    return ms < 0.0 ? std::wstring(L"not followed yet") : std::format(L"{:.2f} ms", ms);
}

/// The engine's numbers of the last frame, for a hitch's line.
std::wstring EngineNow()
{
    if (g_engine[Game] == nullptr)
    {
        return L"the engine's numbers are not read (a check failed)";
    }
    std::wstring text = L"the engine's last frame:";
    for (std::size_t i = 0; i < kEngineCount; ++i)
    {
        text += std::format(L"{} {} {:.1f}", i == 0 ? L"" : L",", kEngineNames[i], g_numbers[i].lastMs);
    }
    return text + L" ms";
}

/// Ends the window at `now`: its counts are taken and the next window started first, then its line is written (a line
/// that fails to format loses only itself).
void EndWindow(std::int64_t now)
{
    const double                           seconds    = static_cast<double>(now - g_windowStart) * g_msPerTick / 1000.0;
    const std::uint32_t                    frames     = g_frames;
    const std::uint32_t                    hitches    = g_hitches;
    const double                           frameSum   = g_frameSumMs;
    const double                           frameWorst = g_frameWorstMs;
    const std::array<Number, kEngineCount> numbers    = g_numbers;
    const double gameCpu   = EndThreadWindow(g_threads[GameThread], GetCurrentThreadId(), frames);
    const double renderCpu = EndThreadWindow(g_threads[RenderThread], ThreadIdOf(g_renderingThread), frames);
    const double rhiCpu    = EndThreadWindow(g_threads[RhiThread], ThreadIdOf(g_rhiThread), frames);
    const bool   rhiOwn    = g_rhiThread != nullptr && g_threads[RhiThread].id != 0;
    g_windowStart  = now;
    g_frames       = 0;
    g_hitches      = 0;
    g_frameSumMs   = 0.0;
    g_frameWorstMs = 0.0;
    for (Number& n : g_numbers)
    {
        n.sumMs   = 0.0;
        n.worstMs = 0.0;
        n.changed = false;
    }

    std::wstring engine;
    if (g_engine[Game] != nullptr)
    {
        engine = L"; the engine's per frame, average / worst:";
        for (std::size_t i = 0; i < kEngineCount; ++i)
        {
            engine += std::format(L"{} {} {:.2f} / {:.1f} ms", i == 0 ? L"" : L",", kEngineNames[i],
                                  frames != 0 ? numbers[i].sumMs / frames : 0.0, numbers[i].worstMs);
        }
        if (!numbers[Gpu].changed)
        {
            engine += L" (the GPU's unchanged all window: stale, the engine's recorder of it is off)";
        }
    }
    g_last = std::format(L"{:.1f} s, {} frames, {:.1f} fps: frame {:.2f} ms average, {:.1f} ms worst, {} hitch(es){}; CPU time "
                         L"per frame (Windows): game thread {}, render thread {}, RHI thread {}",
                         seconds, frames, seconds > 0.0 ? frames / seconds : 0.0, frames != 0 ? frameSum / frames : 0.0, frameWorst,
                         hitches, engine, Ms(gameCpu), Ms(renderCpu),
                         rhiOwn ? Ms(rhiCpu) : std::wstring(L"none (no thread of its own: its work runs on the task threads)"));
    platform::LogDebug(L"frame watch: {}", g_last);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    g_msPerTick = 1000.0 / static_cast<double>(platform::TicksPerSecond());  // the engine's ticks are this counter's
    std::array<std::optional<std::uintptr_t>, kEngineCount> engine{};
    for (std::size_t i = 0; i < kEngineCount; ++i)
    {
        engine[i] = image.Rebase(kEngineGlobals[i], sizeof(std::uint32_t));
    }
    const auto rendering = image.Rebase(facts::kRenderingThread, sizeof(void*));
    const auto rhi       = image.Rebase(facts::kRhiThread, sizeof(void*));
    const bool engineOk  = platform::FirstDiffering(image, kTimeSites) == nullptr && engine[Game] && engine[Render] &&
                          engine[Present] && engine[Gpu];
    const bool threadsOk = platform::FirstDiffering(image, kThreadSites) == nullptr && rendering && rhi;
    if (engineOk)
    {
        for (std::size_t i = 0; i < kEngineCount; ++i)
        {
            g_engine[i] = reinterpret_cast<const std::uint32_t*>(*engine[i]);
        }
    }
    if (threadsOk)
    {
        g_renderingThread = reinterpret_cast<const void* const*>(*rendering);
        g_rhiThread       = reinterpret_cast<const void* const*>(*rhi);
    }
    control::Register({.name  = L"frame-watch.window",
                       .shape = control::Shape::Integer,
                       .min   = kShortestWindow,
                       .max   = kWindowSeconds,
                       .help  = L"the frame watch's window in seconds (10 at start; 1 to bracket an attack of a few seconds): one "
                                L"Debug line per window with the frame rate, the worst frame, the hitches, the engine's own "
                                L"per-thread numbers and each frame thread's CPU time",
                       .set   = [](const control::Value& v) {
                           g_windowSeconds = std::get<std::int64_t>(v);
                           return control::Outcome{true, std::format(L"windows of {} s from the next", g_windowSeconds)};
                       },
                       .get = [] { return control::Value{g_windowSeconds}; }});
    control::Register({.name  = L"frame-watch",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the frame watch's last window: frame rate, average and worst frame, hitches, the "
                                L"engine's own per-thread numbers and each frame thread's CPU time",
                       .get   = [] { return control::Value{g_last}; }});
    g_installed = true;
    platform::Log(L"frame watch: installed; a Debug line every {} s with the frame rate, the worst frame and the hitches{}{} "
                  L"(frame-watch.window sets 1 to 10 s)",
                  g_windowSeconds,
                  engineOk ? L", the engine's own game, render, present and GPU times"
                           : L"; the engine's own times left out (a check failed)",
                  threadsOk ? L", and the game, render and RHI threads' CPU time"
                            : L", and the game thread's CPU time (the render and RHI threads' check failed)");
    return true;
}

void OnFrameStart() noexcept
try
{
    if (!g_installed)
    {
        return;
    }
    const std::int64_t now = platform::Ticks();
    if (g_lastTick == 0)
    {
        g_lastTick = g_windowStart = now;
        return;
    }
    const double frameMs = static_cast<double>(now - g_lastTick) * g_msPerTick;
    g_lastTick           = now;
    ++g_frames;
    g_frameSumMs += frameMs;
    g_frameWorstMs = frameMs > g_frameWorstMs ? frameMs : g_frameWorstMs;
    for (std::size_t i = 0; i < kEngineCount && g_engine[i] != nullptr; ++i)
    {
        std::uint32_t raw = 0;
        if (!platform::Read(g_engine[i], raw))
        {
            continue;
        }
        Number& n = g_numbers[i];
        n.lastMs  = raw * g_msPerTick;
        n.sumMs += n.lastMs;
        n.worstMs = n.lastMs > n.worstMs ? n.lastMs : n.worstMs;
        n.changed = n.changed || raw != n.lastRaw;
        n.lastRaw = raw;
    }
    if (platform::HitchStep(g_hitch, frameMs, kHitchRatio, kHitchMarginMs, kNewNormalRun, kAverageWeight) ==
        platform::FrameKind::Hitch)
    {
        ++g_hitches;
        platform::LogDebug(L"frame watch: hitch: frame {:.1f} ms (average {:.1f}); {}", frameMs, g_hitch.averageMs, EngineNow());
    }
    if (static_cast<double>(now - g_windowStart) * g_msPerTick >= static_cast<double>(g_windowSeconds) * 1000.0)
    {
        EndWindow(now);
    }
}
catch (...)
{
}

}  // namespace lss::render::frame_watch
