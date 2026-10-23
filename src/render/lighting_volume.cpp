#include "render/lighting_volume.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "render/view_family.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace lss::render::lighting_volume {

namespace {

using BoundsFn = std::uintptr_t (*)(const void* view, void* out);

// Set once at install, on the game thread, before the redirect is live; read on the render thread afterwards.
platform::CallRedirect g_boundsCall;
BoundsFn               g_bounds = nullptr;

std::atomic<bool>          g_installed{false};
std::atomic<bool>          g_box{false};  // the test: the further views take player 1's box
std::atomic<std::uint64_t> g_borrowed{0};

/// The first view of the view's family, if the family lists the view and has more than one; null otherwise.
const void* FirstOfSeveral(const void* view) noexcept
{
    const view_family::Family family = view_family::Of(view);
    return family.count >= 2 ? view_family::First(family) : nullptr;
}

/// The view's volume box, from the game's own computation: for the test, a further view's box is the first view's.
std::uintptr_t Bounds(const void* view, void* out)
{
    if (g_box.load(std::memory_order_relaxed))
    {
        const void* const first = FirstOfSeveral(view);
        if (first != nullptr && first != view)
        {
            g_borrowed.fetch_add(1, std::memory_order_relaxed);
            return g_bounds(first, out);
        }
    }
    return g_bounds(view, out);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto bounds = platform::FindFunction(image, facts::kCalcVolumeBounds);
    const auto site   = image.Rebase(facts::kCalcVolumeBoundsCall, 5);
    if (!bounds || !site)
    {
        platform::Log(L"lighting volume: an engine check failed; the volume-box test is not available");
        return false;
    }
    g_bounds = reinterpret_cast<BoundsFn>(*bounds);
    if (!g_boundsCall.Install(*site, *bounds, reinterpret_cast<std::uintptr_t>(&Bounds)))
    {
        platform::Log(L"lighting volume: the box call is not the call the research read; the volume-box test is not "
                      L"available");
        return false;
    }
    g_installed.store(true);
    control::Register({.name  = L"volume-box",
                       .shape = control::Shape::OnOff,
                       .help  = L"test: players 2 and 3 read the lighting volume with player 1's box, the game's stereo "
                                L"rule (render/lighting_volume.h; off)",
                       .set   = [](const control::Value& value) {
                           g_box.store(std::get<bool>(value), std::memory_order_relaxed);
                           platform::Log(L"lighting volume: {}", g_box.load()
                                                                      ? L"the further views take player 1's box (test)"
                                                                      : L"each view takes its own box (the game's)");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_box.load()}; }});
    control::Register({.name  = L"volume-box.views",
                       .shape = control::Shape::Text,
                       .help  = L"view setups that took player 1's lighting volume box since the start",
                       .get   = [] {
                           std::wstring text;
                           try
                           {
                               text = std::to_wstring(g_borrowed.load());
                           }
                           catch (...)
                           {
                           }
                           return control::Value{text};
                       }});
    platform::Log(L"lighting volume: installed; each view takes its own box (switch volume-box tests player 1's)");
    return true;
}

void Remove() noexcept
{
    if (!g_installed.exchange(false))
    {
        return;
    }
    g_boundsCall.Restore();
}

}  // namespace lss::render::lighting_volume
