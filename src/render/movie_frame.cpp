#include "render/movie_frame.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <atomic>
#include <cstdint>
#include <format>
#include <string>

namespace lss::render::movie_frame {

namespace {

using ReleaseFn = void (*)(void* sceneTargets);

platform::CallRedirect g_call;
std::uintptr_t         g_member    = 0;  // &FSceneRenderTargets+0x120, for the reading
bool                   g_installed = false;
std::atomic<bool>      g_enabled{true};

// Render thread only.
bool          g_viewIsLast = true;  // the game's behaviour until the wrapper says otherwise
std::uint32_t g_deferred   = 0;     // releases skipped for a view before the last
std::uint32_t g_released   = 0;     // releases run with a frame in the member
bool          g_logged     = false; // the first deferral of a movie, logged once per movie

/// The composite source now (the movie frame, or null outside a movie).
void* Source() noexcept
{
    void* value = nullptr;
    platform::CopyGuarded(&value, reinterpret_cast<const void*>(g_member), sizeof(value));
    return value;
}

/// SceneRenderTargets__ReleaseCompositeHDR_120, for one view's end of post-processing.
void Release(void* sceneTargets)
{
    const bool  on    = g_enabled.load(std::memory_order_relaxed);
    void* const frame = Source();
    if (frame == nullptr)
    {
        g_logged = false;  // no movie: the next movie's first deferral is logged again
    }
    if (on && !g_viewIsLast)
    {
        ++g_deferred;
        if (frame != nullptr)
        {
            // Once per movie at Info (the latch holds until the source is gone), every frame at Trace.
            if (!g_logged)
            {
                g_logged = true;
                platform::Log(L"movie frame: kept for the next view (the composite source {} is released after the family's last view)",
                              frame);
            }
            platform::LogTrace(L"movie frame: deferred for a view before the last (source {})", frame);
        }
        return;
    }
    if (frame != nullptr)
    {
        ++g_released;
    }
    reinterpret_cast<ReleaseFn>(g_call.Original())(sceneTargets);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto site   = image.Rebase(facts::kCompositeReleaseCall, 5);
    const auto target = platform::FindFunction(image, facts::kCompositeRelease);
    const auto member = image.Rebase(facts::kSceneRenderTargetsGlobal + facts::kSceneTargetsCompositeHdr, sizeof(void*));
    if (!site || !target || !member)
    {
        platform::Log(L"movie frame: the release differs from the research; a movie shows in the first view only");
        return false;
    }
    g_member = *member;
    if (!g_call.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&Release)))
    {
        platform::Log(L"movie frame: the release call is not what the research read; a movie shows in the first view only");
        return false;
    }
    g_installed = true;
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"movie-frame",
              .shape = Shape::OnOff,
              .help  = L"a pre-rendered movie drawn in every view: the game's release of the movie frame runs after the "
                       L"last view instead of after each (off: the second view's movie is black)",
              .set   = [](const Value& v) {
                  SetEnabled(std::get<bool>(v));
                  platform::Log(L"movie frame: {}", Enabled() ? L"kept for every view" : L"released after the first view (the game's way)");
                  return control::Outcome{};
              },
              .get = [] { return Value{Enabled()}; }});
    Register({.name = L"movie-frame.state", .shape = Shape::Text, .help = L"the movie frame: the source now, releases deferred and run",
              .get  = [] {
                  return Value{std::format(L"{}; source {}; releases deferred {}, run {}", Enabled() ? L"on" : L"off", Source(),
                                           g_deferred, g_released)};
              }});
    platform::Log(L"movie frame: ready (a movie draws in every view: its frame is released after the family's last view)");
    return true;
}

void Remove() noexcept
{
    if (g_installed && !g_call.Restore())
    {
        platform::LogWarning(L"movie frame: the release call could not be put back");
    }
    g_installed = false;
}

void BeginView(bool last) noexcept
{
    g_viewIsLast = last;
}

void EndView() noexcept
{
    g_viewIsLast = true;
}

void SetEnabled(bool on) noexcept
{
    g_enabled.store(on, std::memory_order_relaxed);
}

bool Enabled() noexcept
{
    return g_enabled.load(std::memory_order_relaxed);
}

}  // namespace lss::render::movie_frame
