#include "hud/second_image.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/image_code.h"
#include "hud/image_entries.h"
#include "hud/overlay_pass.h"
#include "hud/pass_hide.h"
#include "hud/ui_targets.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>
#include <cstring>

namespace lss::hud::second_image {

namespace {

using RenderFn  = void (*)(void* overlay);
using AcquireFn = void* (*)(void* renderer, std::uintptr_t pool);
using SubmitFn  = void (*)(void* renderer, std::uintptr_t pool, void* slot);

platform::VTableEntry  g_driver;       // the overlay render delegate's slot
platform::CallRedirect g_acquireCall;  // crosses a block: installed from the game thread
platform::CodeRedirect g_naviCode;
RenderFn               g_render  = nullptr;
SubmitFn               g_submit  = nullptr;
std::uintptr_t         g_acquireSite = 0, g_acquireTarget = 0;
double*                g_deltaTime = nullptr;  // FApp::DeltaTime
bool                   g_installed = false;
bool                   g_acquirePending = false;

// Game thread only.
DWORD                   g_gameThread = 0;
bool                    g_on         = true;
bool                    g_split      = false;
bool                    g_refused    = false;
alignas(8) std::uint8_t g_naviSkip   = 0;  // read by the navi map's code (hud/image_code.h)
void*                   g_renderer   = nullptr;  // from player 2's acquire
void*                   g_slot       = nullptr;  // player 2's acquired slot, for player 1's pass
unsigned long long      g_stockName  = 0;
unsigned long long      g_twoName    = 0;
bool                    g_running    = false;    // the last frame rendered player 2's image (logged on change)

void Refuse(const wchar_t* why)
{
    g_refused = true;
    platform::Log(L"HUD image: {}; player 2's image stays off", why);
}

bool Ready() noexcept
{
    if (!g_on || !g_split || g_refused || g_twoName == 0 || !g_acquireCall.IsInstalled() || !g_naviCode.IsInstalled())
    {
        return false;
    }
    if (!image_entries::BuffersReady())
    {
        image_entries::RequestBuffers();
        return false;
    }
    return ui_targets::Enabled();
}

/// The overlay render with player 2's levers (DeltaTime 0, the fade kept). False if the render faulted.
bool RenderPlayerTwo(void* overlay) noexcept
{
    const double deltaTime = *g_deltaTime;
    std::uint8_t fade[20]{};
    platform::CopyGuarded(fade, static_cast<const std::uint8_t*>(overlay) + facts::kOverlayFade, sizeof(fade));
    pass_hide::BeginPlayerTwoPass();
    *g_deltaTime = 0.0;
    g_naviSkip   = 1;
    overlay_pass::Set(overlay_pass::Pass::PlayerTwo);
    const bool ok = platform::CallGuarded(g_render, overlay);
    g_naviSkip   = 0;
    *g_deltaTime = deltaTime;
    platform::WriteGuarded(static_cast<std::uint8_t*>(overlay) + facts::kOverlayFade, fade, sizeof(fade));
    pass_hide::EndPlayerTwoPass();
    return ok;
}

/// Replaces the overlay render delegate (the game's thunk: render the overlay if there is one).
void Driver(void* delegate)
{
    void* overlay = nullptr;
    platform::CopyGuarded(&overlay, static_cast<const std::uint8_t*>(delegate) + 8, sizeof(overlay));
    if (overlay == nullptr)
    {
        return;
    }
    const bool two = GetCurrentThreadId() == g_gameThread && Ready();
    if (two != g_running)
    {
        g_running = two;
        platform::Log(L"HUD image: player 2's image {}", two ? L"rendered each frame" : L"stopped");
    }
    if (two && !RenderPlayerTwo(overlay))
    {
        Refuse(L"the overlay render faulted in player 2's pass");
    }
    overlay_pass::Set(two ? overlay_pass::Pass::PlayerOne : overlay_pass::Pass::Single);
    g_render(overlay);
    overlay_pass::Set(overlay_pass::Pass::Single);
    if (g_slot != nullptr)
    {
        // Player 1's pass did not take the slot (its render returned early). The acquire advanced the ring, so the slot
        // is submitted as the game's own pass would have, holding player 2's entry alone (Rebirth's lesson).
        g_submit(g_renderer, 2, g_slot);
        g_slot = nullptr;
        platform::LogDebug(L"HUD image: player 1's pass did not render; player 2's slot submitted on its own");
    }
}

/// Replaces the render's call to FEndMenuRenderer__AcquireLayerSlot (pool 2).
void* Acquire(void* renderer, std::uintptr_t pool)
{
    const auto pass = overlay_pass::Current();
    if (pass == overlay_pass::Pass::PlayerOne && g_slot != nullptr)
    {
        void* slot = g_slot;
        g_slot     = nullptr;
        return slot;  // the slot player 2's pass acquired: one acquire, one submit a frame
    }
    void* slot = reinterpret_cast<AcquireFn>(g_acquireCall.Original())(renderer, pool);
    if (pass == overlay_pass::Pass::PlayerTwo)
    {
        g_renderer = renderer;
        g_slot     = slot;
    }
    return slot;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    namespace c          = image_code;
    const auto render    = platform::FindFunction(image, facts::kOverlayRender);
    const auto slot      = image.Rebase(facts::kOverlayDelegateSlot, sizeof(void*));
    const auto acquire   = image.Rebase(facts::kOverlayAcquireCall, 5);
    const auto acquireFn = image.Rebase(c::kAcquireLayer);
    const auto submitFn  = image.Rebase(facts::kSubmitLayerSlot);
    const auto delta     = image.Rebase(facts::kAppDeltaTime, sizeof(double));
    const auto thunk     = image.Rebase(facts::kOverlayDelegateThunk);
    const auto navi      = image.Rebase(c::kNaviSite, sizeof(c::kNaviCall));
    const auto naviBack  = image.Rebase(c::kNaviReturn);
    const auto naviSkip  = image.Rebase(c::kNaviSkipTo);
    if (!render || !slot || !acquire || !acquireFn || !submitFn || !delta || !thunk || !navi || !naviBack || !naviSkip ||
        !image.BytesMatch(c::kNaviCheckStart, c::kNaviBytes) || !image.BytesMatch(c::kNaviSubmitStart, c::kNaviSubmitBytes))
    {
        platform::Log(L"HUD image: the overlay render is not what the research read; player 2's image stays off");
        return false;
    }
    g_render        = reinterpret_cast<RenderFn>(*render);
    g_submit        = reinterpret_cast<SubmitFn>(*submitFn);
    g_deltaTime     = reinterpret_cast<double*>(*delta);
    g_acquireSite   = *acquire;
    g_acquireTarget = *acquireFn;
    if (!g_naviCode.Install(*navi, c::kNaviCall, c::NaviCode(g_naviSkip, *naviBack, *acquireFn, *naviSkip), *naviBack))
    {
        platform::Log(L"HUD image: the navi map's acquire could not be redirected; player 2's image stays off");
        return false;
    }
    if (!g_driver.Replace(*slot, *thunk, reinterpret_cast<std::uintptr_t>(&Driver)))
    {
        g_naviCode.Restore();
        platform::Log(L"HUD image: the overlay's render delegate is not the game's thunk; player 2's image stays off");
        return false;
    }
    g_acquirePending = true;
    g_installed      = true;

    using control::Outcome;
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name  = L"hud-image",
              .shape = Shape::OnOff,
              .help  = L"player 2's own HUD image: the overlay rendered first for player 2 (on at start; turns hud-targets on)",
              .set   = [](const Value& value) {
                  SetEnabled(std::get<bool>(value));
                  return Outcome{};
              },
              .get = [] { return Value{Enabled()}; }});
    platform::Log(L"HUD image: installed (hud-image on); the acquire call follows on the game thread");
    return true;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_on = false;
    const bool driver  = g_driver.Restore();
    const bool navi    = g_naviCode.Restore();
    const bool acquire = !g_acquireCall.IsInstalled() || (GetCurrentThreadId() == g_gameThread && g_acquireCall.Restore());
    if (!driver || !navi || !acquire)
    {
        platform::Log(L"HUD image: a redirect was changed after us or is off the game thread; left in place (inert: the "
                      L"image is off)");
    }
    g_installed = !(driver && navi && acquire);
}

void Tick(bool splitShown) noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_gameThread = GetCurrentThreadId();
    if (g_acquirePending)
    {
        g_acquirePending = false;  // outside the overlay render here, as InstallFromOwnThread requires
        const bool ok = g_acquireCall.InstallFromOwnThread(g_acquireSite, g_acquireTarget,
                                                           reinterpret_cast<std::uintptr_t>(&Acquire));
        platform::Log(L"HUD image: the overlay's acquire call {}",
                      ok ? L"installed from the game thread" : L"could not be redirected (player 2's image stays off)");
    }
    g_split = splitShown;
    if (g_on && splitShown && !ui_targets::Enabled())
    {
        ui_targets::SetEnabled(true);  // the image needs each view's own target
    }
}

void SetEnabled(bool on) noexcept
{
    g_on = on;
    if (on)
    {
        g_refused = false;
    }
}

bool Enabled() noexcept
{
    return g_on && !g_refused;
}

unsigned long long EntryNameForPass(unsigned long long stockName) noexcept
{
    if (overlay_pass::Current() != overlay_pass::Pass::PlayerTwo)
    {
        if (g_stockName == 0 && stockName != 0)
        {
            // The stock entry name with another number: a distinct FName to the entry search's 8-byte compare, which
            // needs no registration (RE-hud-overlay-pass.md §3.3). Number is the FName's high dword.
            g_stockName = stockName;
            g_twoName   = stockName + (2ull << 32);
            platform::LogDebug(L"HUD image: player 2's entry name {:#x} (the stock {:#x}, number 2)", g_twoName, g_stockName);
        }
        return stockName;
    }
    return g_twoName != 0 ? g_twoName : stockName;
}

}  // namespace lss::hud::second_image
