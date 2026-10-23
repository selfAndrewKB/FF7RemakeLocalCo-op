#include "render/sun_cascades.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "render/sun_owner.h"
#include "render/view_family.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <intrin.h>
#include <span>
#include <string>
#include <vector>

namespace lss::render::sun_cascades {

namespace {

using CascadeTestFn = bool (*)(const void* shadow);
using InjectFn      = std::uintptr_t (*)(void* commandList, const void* view, const void* lights);

/// The injection list as the game lays it out: TArray {Data, Num, Max}.
struct InjectionList
{
    const sun_owner::Injection* data;
    std::int32_t                num;
    std::int32_t                max;
};

// Set once at install, on the game thread, before any redirect is live; read on the render thread afterwards (the
// redirects' atomic 16-byte code swaps order the writes before any read, x64).
platform::CallRedirect                g_cascadeTestCall;
std::array<platform::CallRedirect, 4> g_injectCalls;
CascadeTestFn                         g_cascadeTest = nullptr;
InjectFn                              g_inject      = nullptr;

std::atomic<bool>          g_installed{false};
std::atomic<bool>          g_enabled{true};
std::atomic<std::uint64_t> g_lightDataAway{0};  // cascades refused to another view's forward light data
std::atomic<std::uint64_t> g_volumeAway{0};     // shadowed entries left out of the lighting volume's injection
std::atomic<bool>          g_loggedLightData{false};
std::atomic<bool>          g_loggedVolume{false};
std::atomic<bool>          g_loggedNoView{false};

/// The view a shadow was made for; 0 if it has none or the read faulted.
std::uintptr_t OwnerOf(std::uintptr_t shadow) noexcept
{
    std::uintptr_t owner = 0;
    return platform::Read(reinterpret_cast<const std::uint8_t*>(shadow) + facts::kShadowDependentView, owner) ? owner : 0;
}

/// The forward light data's cascade test, for the view being filled: the game's own test, then no for a cascade made
/// for another view. Called from the game's loop with the shadow in rcx; the view is in the caller's frame.
bool CascadeTest(const void* shadow)
{
    if (!g_cascadeTest(shadow))
    {
        return false;
    }
    if (!g_enabled.load(std::memory_order_relaxed))
    {
        return true;
    }
    // The caller's rsp at its call: one slot above our return address. This holds only because the redirect's thunk
    // is a bare jump (platform/call_redirect.h: no push, no frame); the caller's frame was checked at install.
    const auto* const callerStack = static_cast<const std::uint8_t*>(_AddressOfReturnAddress()) + sizeof(void*);
    const auto        view = *reinterpret_cast<const std::uintptr_t*>(callerStack + facts::kLightGridFilledView);
    // Only a live view of the frame is trusted: anything else, and the cascade is left to the game (one cascade too
    // many at worst, never every view's own sun shadow gone).
    if (view_family::Of(reinterpret_cast<const void*>(view)).count == 0)
    {
        if (!g_loggedNoView.exchange(true))
        {
            platform::LogWarning(L"sun cascades: the forward light data's frame does not hold a view (0x{:X}); its "
                                 L"cascades are left to the game", view);
        }
        return true;
    }
    const std::uintptr_t owner = OwnerOf(reinterpret_cast<std::uintptr_t>(shadow));
    if (!sun_owner::OtherView(owner, view))
    {
        return true;
    }
    g_lightDataAway.fetch_add(1, std::memory_order_relaxed);
    if (!g_loggedLightData.exchange(true))
    {
        platform::Log(L"sun cascades: a view's forward light data was offered another view's sun cascade (view 0x{:X}, "
                      L"the cascade's 0x{:X}); refused, as from now on (counted in the reading sun-cascades.away)",
                      view, owner);
    }
    return false;
}

/// One call of the lighting volume's light injection: the game's list, less the entries whose shadow is another
/// view's cascade. The list goes as it is when it has none (one view always).
std::uintptr_t Inject(void* commandList, const void* view, const InjectionList* lights)
{
    if (!g_enabled.load(std::memory_order_relaxed) || lights == nullptr || lights->data == nullptr || lights->num <= 0)
    {
        return g_inject(commandList, view, lights);
    }
    const std::span<const sun_owner::Injection> in(lights->data, static_cast<std::size_t>(lights->num));
    const auto                                  viewAddress = reinterpret_cast<std::uintptr_t>(view);
    if (!sun_owner::AnyOther(in, viewAddress, OwnerOf))
    {
        return g_inject(commandList, view, lights);
    }
    thread_local std::vector<sun_owner::Injection> kept;
    std::size_t                                    dropped = 0;
    try
    {
        dropped = sun_owner::Keep(in, viewAddress, OwnerOf, kept);
    }
    catch (...)
    {
        return g_inject(commandList, view, lights);  // out of memory: the game's list as it is
    }
    g_volumeAway.fetch_add(dropped, std::memory_order_relaxed);
    if (!g_loggedVolume.exchange(true))
    {
        platform::Log(L"sun cascades: the lighting volume's injection was given {} of {} lights with another view's "
                      L"cascade (view 0x{:X}); left out, as from now on (counted in the reading sun-cascades.away)",
                      dropped, in.size(), viewAddress);
    }
    // An empty list is the game's own "no light" case: the injection loops over nothing.
    const InjectionList copy{kept.data(), static_cast<std::int32_t>(kept.size()), static_cast<std::int32_t>(kept.size())};
    return g_inject(commandList, view, &copy);
}

/// Puts every installed call back; a call changed after the fix is left as it is (logged).
void RestoreAll() noexcept
{
    for (std::size_t i = 0; i < g_injectCalls.size(); ++i)
    {
        if (g_injectCalls[i].IsInstalled() && !g_injectCalls[i].Restore())
        {
            platform::LogWarning(L"sun cascades: injection call {} was changed after the fix; left as it is", i + 1);
        }
    }
    if (g_cascadeTestCall.IsInstalled() && !g_cascadeTestCall.Restore())
    {
        platform::LogWarning(L"sun cascades: the forward light data's cascade test was changed after the fix; left as it is");
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto cascadeTest = platform::FindFunction(image, facts::kIsWholeSceneDirectionalShadow);
    const auto inject      = platform::FindFunction(image, facts::kInjectVolumeLighting);
    const auto testCall    = image.Rebase(facts::kLightGridCascadeTestCall, 5);
    if (!cascadeTest || !inject || !testCall ||
        !image.BytesMatch(facts::kLightGridFrameAlloc, facts::kLightGridFrameAllocBytes) ||
        !image.BytesMatch(facts::kLightGridViewStore, facts::kLightGridViewStoreBytes))
    {
        platform::Log(L"sun cascades: an engine check failed; with more than one view, each view keeps taking every "
                      L"view's sun cascades");
        return false;
    }
    g_cascadeTest = reinterpret_cast<CascadeTestFn>(*cascadeTest);
    g_inject      = reinterpret_cast<InjectFn>(*inject);
    bool installed = g_cascadeTestCall.Install(*testCall, *cascadeTest, reinterpret_cast<std::uintptr_t>(&CascadeTest));
    for (std::size_t i = 0; installed && i < g_injectCalls.size(); ++i)
    {
        const auto site = image.Rebase(facts::kInjectVolumeLightingCalls[i], 5);
        installed = site && g_injectCalls[i].Install(*site, *inject, reinterpret_cast<std::uintptr_t>(&Inject));
    }
    if (!installed)
    {
        RestoreAll();
        platform::Log(L"sun cascades: a call site is not the call the research read; with more than one view, each view "
                      L"keeps taking every view's sun cascades");
        return false;
    }
    g_installed.store(true);
    control::Register({.name  = L"sun-cascades",
                       .shape = control::Shape::OnOff,
                       .help  = L"each view's forward light data and the lighting volume take the sun cascades of the "
                                L"view they are for only (render/sun_cascades.h; on)",
                       .set   = [](const control::Value& value) {
                           g_enabled.store(std::get<bool>(value), std::memory_order_relaxed);
                           g_loggedLightData.store(false);  // the first refusal after a change is logged again
                           g_loggedVolume.store(false);
                           platform::Log(L"sun cascades fix {}", g_enabled.load() ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_enabled.load()}; }});
    control::Register({.name  = L"sun-cascades.away",
                       .shape = control::Shape::Text,
                       .help  = L"another view's sun cascades turned away since the start: from a view's forward light "
                                L"data, and from the lighting volume's injection",
                       .get   = [] {
                           std::wstring text;
                           try
                           {
                               text = std::format(L"forward light data {}; lighting volume {}", g_lightDataAway.load(),
                                                  g_volumeAway.load());
                           }
                           catch (...)
                           {
                           }
                           return control::Value{text};
                       }});
    platform::Log(L"sun cascades: installed; each view's forward light data and the lighting volume will take only the "
                  L"sun cascades of the view they are for");
    return true;
}

void Remove() noexcept
{
    if (!g_installed.exchange(false))
    {
        return;
    }
    RestoreAll();
}

}  // namespace lss::render::sun_cascades
