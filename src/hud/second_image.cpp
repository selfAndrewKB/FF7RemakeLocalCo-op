#include "hud/second_image.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/image_code.h"
#include "hud/image_entries.h"
#include "hud/field_icons.h"
#include "hud/marker_replay.h"
#include "hud/minimap_two.h"
#include "hud/overlay_layout.h"
#include "hud/overlay_pass.h"
#include "battle/command_menu_two.h"
#include "battle/target_cursor_two.h"
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
#include <format>
#include <string>

namespace lss::hud::second_image {

namespace {

using RenderFn  = void (*)(void* overlay);
using AcquireFn = void* (*)(void* renderer, std::uintptr_t pool);
using SubmitFn  = void (*)(void* renderer, std::uintptr_t pool, void* slot);

platform::VTableEntry  g_driver;       // the overlay render delegate's slot
platform::CallRedirect g_acquireCall;  // crosses a block: installed from the game thread
RenderFn               g_render  = nullptr;
SubmitFn               g_submit  = nullptr;
std::uintptr_t         g_acquireSite = 0, g_acquireTarget = 0;
double*                g_deltaTime = nullptr;  // FApp::DeltaTime
void**                 g_uiFilters = nullptr;  // &EndUISystem[37] (the UI filters: type 4 fades the whole overlay)
float                  g_fadeTarget = -1.0f;   // the overlay fade's target last frame (an Info line when it changes)
bool                   g_installed = false;
bool                   g_acquirePending = false;

// Game thread only.
DWORD                   g_gameThread = 0;
bool                    g_on         = true;
bool                    g_split      = false;
bool                    g_refused    = false;
void*                   g_renderer   = nullptr;  // from player 2's acquire
void*                   g_slot       = nullptr;  // player 2's acquired slot, for player 1's pass
unsigned long long      g_stockName  = 0;
unsigned long long      g_twoName    = 0;
bool                    g_running    = false;    // the last frame rendered player 2's image (logged on change)
Players                 g_players;

/// Each pass's cost, averaged over the frames since the reading was last taken (game thread).
struct Timing
{
    double        twoMs = 0.0, oneMs = 0.0;
    std::uint32_t frames = 0;
};
Timing g_timing;
double g_lastTwoMs = 0.0, g_lastOneMs = 0.0;

double NowMs() noexcept
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(counter.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

void Refuse(const wchar_t* why)
{
    g_refused = true;
    platform::Log(L"HUD image: {}; player 2's image stays off", why);
}

bool Ready() noexcept
{
    if (!g_on || !g_split || g_refused || g_twoName == 0 || !g_acquireCall.IsInstalled() ||
        !minimap_two::PoolCodeInstalled())
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

/// The UI filters' counts and suppressed flags, for the fade lines.
std::wstring Filters() noexcept
{
    std::uint8_t* filters = nullptr;
    std::int32_t  counts[6]{};
    std::uint8_t  suppressed[6]{};
    if (g_uiFilters == nullptr || !platform::CopyGuarded(&filters, g_uiFilters, sizeof(filters)) || filters == nullptr ||
        !platform::CopyGuarded(counts, filters + facts::kUiFilterCounts, sizeof(counts)) ||
        !platform::CopyGuarded(suppressed, filters + facts::kUiFilterSuppressed, sizeof(suppressed)))
    {
        return L"unreadable";
    }
    return std::format(L"counts {} {} {} {} {} {}, suppressed {}{}{}{}{}{}", counts[0], counts[1], counts[2], counts[3],
                       counts[4], counts[5], suppressed[0], suppressed[1], suppressed[2], suppressed[3], suppressed[4],
                       suppressed[5]);
}

/// The overlay fade (alpha, from, to, elapsed, duration).
struct Fade
{
    float alpha = 0.0f, from = 0.0f, to = 0.0f, elapsed = 0.0f, duration = 0.0f;
};
static_assert(sizeof(Fade) == 20);

/// An Info line when the game sets a new fade target (UI filter type 4: menus, scripted HUD hides).
void WatchFade(void* overlay) noexcept
{
    Fade fade;
    if (!platform::CopyGuarded(&fade, static_cast<const std::uint8_t*>(overlay) + facts::kOverlayFade, sizeof(fade)) ||
        fade.to == g_fadeTarget)
    {
        return;
    }
    if (g_fadeTarget >= 0.0f)
    {
        platform::Log(L"HUD overlay: the game fades the whole HUD from {:.2f} to {:.2f} over {:.2f} s (UI filters: {})",
                      fade.from, fade.to, fade.duration, Filters());
    }
    g_fadeTarget = fade.to;
}

/// The overlay render with player 2's levers (DeltaTime 0, the fade kept). False if the render faulted.
bool RenderPlayerTwo(void* overlay) noexcept
{
    const double deltaTime = *g_deltaTime;
    Fade         fade;
    platform::CopyGuarded(&fade, static_cast<const std::uint8_t*>(overlay) + facts::kOverlayFade, sizeof(fade));
    pass_hide::BeginPlayerTwoPass();
    battle::command_menu_two::BeginPass(overlay_pass::Pass::PlayerTwo);
    battle::target_cursor_two::BeginPass(overlay_pass::Pass::PlayerTwo);
    *g_deltaTime = 0.0;
    overlay_pass::Set(overlay_pass::Pass::PlayerTwo);
    minimap_two::BeginPass(overlay_pass::Pass::PlayerTwo);
    const bool ok = platform::CallGuarded(g_render, overlay);
    minimap_two::EndPass();
    *g_deltaTime = deltaTime;
    // The fade advances in both renders (by the world's delta), so player 2's step is taken back. A new fade the game
    // asked for during this render (a widget's tick in the paint) is kept: putting the old one back would lose it.
    Fade after;
    platform::CopyGuarded(&after, static_cast<const std::uint8_t*>(overlay) + facts::kOverlayFade, sizeof(after));
    if (after.to != fade.to || after.duration != fade.duration || after.from != fade.from)
    {
        platform::Log(L"HUD image: the game asked for a HUD fade to {:.2f} during player 2's render; kept (UI filters: {})",
                      after.to, Filters());
    }
    else
    {
        platform::WriteGuarded(static_cast<std::uint8_t*>(overlay) + facts::kOverlayFade, &fade, sizeof(fade));
    }
    battle::target_cursor_two::EndPass();
    battle::command_menu_two::EndPass();
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
    if (GetCurrentThreadId() == g_gameThread)
    {
        WatchFade(overlay);
    }
    const bool two = GetCurrentThreadId() == g_gameThread && Ready();
    if (two != g_running)
    {
        g_running = two;
        platform::Log(L"HUD image: player 2's image {}", two ? L"rendered each frame" : L"stopped");
    }
    const double start = NowMs();
    if (two)
    {
        // Markers placed through player 2's camera for player 2's image, then back to player 1's for player 1's.
        const bool hostSets = overlay_layout::WriteHostSet(2);
        if (hostSets)
        {
            marker_replay::Replay(2, g_players.localPlayer[1], g_players.controller[1]);
            field_icons::Replay(2, g_players.controller[1]);
        }
        if (!RenderPlayerTwo(overlay))
        {
            Refuse(L"the overlay render faulted in player 2's pass");
        }
        if (hostSets && overlay_layout::WriteHostSet(1))
        {
            marker_replay::Replay(1, g_players.localPlayer[0], g_players.controller[0]);
            field_icons::Replay(1, g_players.controller[0]);
        }
    }
    const double middle = NowMs();
    overlay_pass::Set(two ? overlay_pass::Pass::PlayerOne : overlay_pass::Pass::Single);
    minimap_two::BeginPass(two ? overlay_pass::Pass::PlayerOne : overlay_pass::Pass::Single);
    battle::command_menu_two::BeginPass(two ? overlay_pass::Pass::PlayerOne : overlay_pass::Pass::Single);
    battle::target_cursor_two::BeginPass(two ? overlay_pass::Pass::PlayerOne : overlay_pass::Pass::Single);
    g_render(overlay);
    battle::target_cursor_two::EndPass();
    battle::command_menu_two::EndPass();
    minimap_two::EndPass();
    overlay_pass::Set(overlay_pass::Pass::Single);
    minimap_two::EndFrame();  // pool 0's slot, if player 1's map did not take it
    if (GetCurrentThreadId() == g_gameThread)  // the replays' records are the game thread's
    {
        marker_replay::EndFrame();
        field_icons::EndFrame();
    }
    const double end = NowMs();
    g_timing.twoMs += middle - start;
    g_timing.oneMs += end - middle;
    if (++g_timing.frames >= 300)
    {
        g_lastTwoMs = g_timing.twoMs / g_timing.frames;
        g_lastOneMs = g_timing.oneMs / g_timing.frames;
        g_timing    = {};
        platform::LogDebug(L"HUD image: overlay render ms per frame over 300 frames: player 2's pass {:.3f}, player 1's {:.3f}",
                           g_lastTwoMs, g_lastOneMs);
    }
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
    if (const auto filters = image.Rebase(facts::kUiFilterSlotGlobal, sizeof(void*)))
    {
        g_uiFilters = reinterpret_cast<void**>(*filters);
    }
    const auto thunk     = image.Rebase(facts::kOverlayDelegateThunk);
    if (!render || !slot || !acquire || !acquireFn || !submitFn || !delta || !thunk || !minimap_two::PoolCodeInstalled())
    {
        platform::Log(L"HUD image: the overlay render is not what the research read; player 2's image stays off");
        return false;
    }
    g_render        = reinterpret_cast<RenderFn>(*render);
    g_submit        = reinterpret_cast<SubmitFn>(*submitFn);
    g_deltaTime     = reinterpret_cast<double*>(*delta);
    g_acquireSite   = *acquire;
    g_acquireTarget = *acquireFn;
    if (!g_driver.Replace(*slot, *thunk, reinterpret_cast<std::uintptr_t>(&Driver)))
    {
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
    Register({.name  = L"hud.image-ms",
              .shape = Shape::Text,
              .help  = L"the overlay render's cost per frame (ms, the last 300 frames): player 2's pass, player 1's pass",
              .get   = [] { return Value{std::format(L"player 2 {:.3f} ms, player 1 {:.3f} ms", g_lastTwoMs, g_lastOneMs)}; }});
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
    const bool acquire = !g_acquireCall.IsInstalled() || (GetCurrentThreadId() == g_gameThread && g_acquireCall.Restore());
    if (!driver || !acquire)
    {
        platform::Log(L"HUD image: a redirect was changed after us or is off the game thread; left in place (inert: the "
                      L"image is off)");
    }
    g_installed = !(driver && acquire);
}

void Tick(bool splitShown, const Players& players) noexcept
{
    g_players = players;
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
