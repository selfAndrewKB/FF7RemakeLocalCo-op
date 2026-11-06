#include "hud/second_image.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "hud/image_code.h"
#include "hud/image_entries.h"
#include "hud/field_icons.h"
#include "hud/marker_replay.h"
#include "hud/minimap.h"
#include "hud/overlay_layout.h"
#include "hud/overlay_pass.h"
#include "hud/outside_icons.h"
#include "battle/command_menu.h"
#include "battle/target_cursor.h"
#include "hud/pass_hide.h"
#include "hud/ui_targets.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/game_thread.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/seats.h"
#include "world/search_arrows.h"
#include "world/target_icons.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::hud::second_image {

namespace {

using RenderFn  = void (*)(void* overlay);
using AcquireFn = void* (*)(void* renderer, std::uintptr_t pool);
using SubmitFn  = void (*)(void* renderer, std::uintptr_t pool, void* slot);

constexpr int kMaxColumns = overlay_pass::kMaxColumns;

platform::VTableEntry  g_driver;       // the overlay render delegate's slot
platform::CallRedirect g_acquireCall;  // crosses a block: installed from the game thread
RenderFn               g_render  = nullptr;
SubmitFn               g_submit  = nullptr;
std::uintptr_t         g_acquireSite = 0, g_acquireTarget = 0;
double*                g_deltaTime = nullptr;  // FApp::DeltaTime
std::uint32_t*         g_frameNumber = nullptr;  // GFrameNumber
void**                 g_uiFilters = nullptr;  // &EndUISystem[37] (the UI filters: type 4 fades the whole overlay)
float                  g_fadeTarget = -1.0f;   // the overlay fade's target last frame (an Info line when it changes)
bool                   g_installed = false;
bool                   g_acquirePending = false;

// Game thread only.
platform::GameThreadMark g_gameThread;
bool                    g_on         = true;
bool                    g_catchUp    = true;     // a further pass does not count as a frame's paint (hud-catch-up)
std::uint32_t           g_catchUpPasses = 0;     // further passes rendered with the frame number taken back
bool                    g_split      = false;
bool                    g_refused    = false;
void*                   g_renderer   = nullptr;  // from the frame's first acquire
void*                   g_slot       = nullptr;  // the slot the frame's first pass acquired, held for the later passes
unsigned long long      g_stockName  = 0;        // the stock entry name, once seen (the columns' names derive from it)
bool                    g_running    = false;    // the last frame rendered the further columns' images (logged on change)
Columns                 g_columns;

/// The passes' cost, averaged over the frames since the reading was last taken (game thread): the further columns'
/// passes together, then player 1's.
struct Timing
{
    double        furtherMs = 0.0, oneMs = 0.0;
    std::uint32_t frames = 0;
};
Timing g_timing;
double g_lastFurtherMs = 0.0, g_lastOneMs = 0.0;

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
    platform::Log(L"HUD image: {}; the further players' images stay off", why);
}

bool Ready() noexcept
{
    if (!g_on || !g_split || g_refused || g_stockName == 0 || g_columns.count < 2 || !g_acquireCall.IsInstalled() ||
        !minimap::PoolCodeInstalled())
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

/// The overlay render as a further column's pass (DeltaTime 0, the fade kept, the per-pass modules told the column).
/// False if the render faulted.
bool RenderFurther(void* overlay, int column) noexcept
{
    const double deltaTime = *g_deltaTime;
    Fade         fade;
    platform::CopyGuarded(&fade, static_cast<const std::uint8_t*>(overlay) + facts::kOverlayFade, sizeof(fade));
    pass_hide::BeginFurtherPass(column);
    battle::command_menu::BeginPass(column);
    battle::target_cursor::BeginPass(column);
    *g_deltaTime = 0.0;
    // The frame number one back, as the engine does around an extra view family: the widgets this pass paints (at a
    // step of 0) are not stamped as painted this frame, so player 1's pass, the last, advances the ones player 1 does
    // not draw by the frame's step through the game's own catch-up (damage numbers off player 1's screen float and
    // expire as in single player instead of freezing).
    const bool rewind = g_catchUp && g_frameNumber != nullptr;
    if (rewind)
    {
        --*g_frameNumber;
        ++g_catchUpPasses;
    }
    overlay_pass::Set(column);
    minimap::BeginPass(column);
    const bool ok = platform::CallGuarded(g_render, overlay);
    minimap::EndPass();
    if (rewind)
    {
        ++*g_frameNumber;  // relative: any step taken during the render is kept
    }
    *g_deltaTime = deltaTime;
    // The fade advances in every render (by the world's delta), so this pass's step is taken back. A new fade the game
    // asked for during this render (a widget's tick in the paint) is kept: putting the old one back would lose it.
    Fade after;
    platform::CopyGuarded(&after, static_cast<const std::uint8_t*>(overlay) + facts::kOverlayFade, sizeof(after));
    if (after.to != fade.to || after.duration != fade.duration || after.from != fade.from)
    {
        platform::Log(L"HUD image: the game asked for a HUD fade to {:.2f} during {}'s render; kept (UI filters: {})",
                      after.to, players::SeatName(overlay_pass::SeatOf(column)), Filters());
    }
    else
    {
        platform::WriteGuarded(static_cast<std::uint8_t*>(overlay) + facts::kOverlayFade, &fade, sizeof(fade));
    }
    battle::target_cursor::EndPass();
    battle::command_menu::EndPass();
    pass_hide::EndPass();
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
    if (g_gameThread.Here())
    {
        WatchFade(overlay);
    }
    const bool further = g_gameThread.Here() && Ready();
    if (further != g_running)
    {
        g_running = further;
        platform::Log(L"HUD image: the further players' images {}", further ? L"rendered each frame" : L"stopped");
    }
    const double start = NowMs();
    if (further)
    {
        // The further columns' passes, the highest column first: the markers placed through that column's camera, then
        // its render; then back to player 1's placements for player 1's render (only needed once a replay moved them),
        // with the host set the game's own placements read this tick put back first.
        overlay_layout::HostSetScope hostSet;
        bool                         replayed = false;
        for (int column = g_columns.count - 1; column >= 1; --column)
        {
            if (hostSet.Lend(column))
            {
                replayed = true;
                marker_replay::Replay(column, g_columns.localPlayer[column], g_columns.controller[column]);
                field_icons::Replay(column, g_columns.controller[column], g_columns.member[column]);
                outside_icons::RunForColumn(column, g_columns.controller[column]);
                const bool mirror = overlay_pass::ShownSeat(column) == 0;
                // The target icons into this view, each through its own player's, a mirror's player 1's through the mirror's.
                world::target_icons::PlaceForPass(mirror ? g_columns.controller[column] : nullptr);
                world::search_arrows::TickForPass(overlay_pass::SeatOf(column));  // the seat's search arrows from its own camera
                if (mirror)
                {
                    world::search_arrows::PlaceForMirror(g_columns.controller[column]);  // player 1's, from the mirror's camera
                }
            }
            if (!RenderFurther(overlay, column))
            {
                Refuse(L"the overlay render faulted in a further column's pass");
                break;
            }
        }
        if (replayed && hostSet.Restore())
        {
            marker_replay::Replay(0, g_columns.localPlayer[0], g_columns.controller[0]);
            field_icons::Replay(0, g_columns.controller[0], nullptr);
            outside_icons::RestorePlayerOne();
            world::search_arrows::RestorePlayerOne();
            world::target_icons::PlaceForPlayerOne();
        }
    }
    const double middle = NowMs();
    const int    own    = further ? 0 : overlay_pass::kSingle;
    // The hides read the game thread's widget list; the overlay renders there in practice (an off-thread render would
    // show the seats' icons once in player 1's view).
    const bool   onGame = g_gameThread.Here();
    if (onGame)
    {
        pass_hide::BeginOwnPass();  // the seats' target icons are not player 1's (nor the single render's)
    }
    overlay_pass::Set(own);
    minimap::BeginPass(own);
    battle::command_menu::BeginPass(own);
    battle::target_cursor::BeginPass(own);
    g_render(overlay);
    battle::target_cursor::EndPass();
    battle::command_menu::EndPass();
    minimap::EndPass();
    if (onGame)
    {
        pass_hide::EndPass();
    }
    overlay_pass::Set(overlay_pass::kSingle);
    minimap::EndFrame();  // pool 0's slot, if player 1's map did not take it
    if (g_gameThread.Here())  // the replays' records are the game thread's
    {
        marker_replay::EndFrame();
        field_icons::EndFrame();
        outside_icons::EndFrame();
    }
    const double end = NowMs();
    g_timing.furtherMs += middle - start;
    g_timing.oneMs += end - middle;
    if (++g_timing.frames >= 300)
    {
        g_lastFurtherMs = g_timing.furtherMs / g_timing.frames;
        g_lastOneMs     = g_timing.oneMs / g_timing.frames;
        g_timing        = {};
        platform::LogDebug(L"HUD image: overlay render ms per frame over 300 frames: the further columns' passes {:.3f}, "
                           L"player 1's {:.3f}",
                           g_lastFurtherMs, g_lastOneMs);
    }
    if (g_slot != nullptr)
    {
        // Player 1's pass did not take the slot (its render returned early). The acquire advanced the ring, so the slot
        // is submitted as the game's own pass would have, holding the further columns' entries alone (Rebirth's lesson).
        g_submit(g_renderer, 2, g_slot);
        g_slot = nullptr;
        platform::LogDebug(L"HUD image: player 1's pass did not render; the held slot submitted on its own");
    }
}

/// Replaces the render's call to FEndMenuRenderer__AcquireLayerSlot (pool 2): one acquire and one submit a frame across
/// the passes (overlay_pass::AcquireAction).
void* Acquire(void* renderer, std::uintptr_t pool)
{
    using overlay_pass::SlotAction;
    switch (overlay_pass::AcquireAction(overlay_pass::Column(), g_slot != nullptr))
    {
    case SlotAction::TakeHeld:
        return g_slot;
    case SlotAction::ConsumeHeld:
    {
        void* slot = g_slot;
        g_slot     = nullptr;  // player 1's pass submits it
        return slot;
    }
    case SlotAction::AcquireAndHold:
    {
        void* slot = reinterpret_cast<AcquireFn>(g_acquireCall.Original())(renderer, pool);
        g_renderer = renderer;
        g_slot     = slot;
        return slot;
    }
    case SlotAction::Acquire:
    default:
        return reinterpret_cast<AcquireFn>(g_acquireCall.Original())(renderer, pool);
    }
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
    const auto frame     = image.Rebase(facts::kFrameNumber, sizeof(std::uint32_t));
    if (const auto filters = image.Rebase(facts::kUiFilterSlotGlobal, sizeof(void*)))
    {
        g_uiFilters = reinterpret_cast<void**>(*filters);
    }
    const auto thunk     = image.Rebase(facts::kOverlayDelegateThunk);
    if (!render || !slot || !acquire || !acquireFn || !submitFn || !delta || !frame || !thunk || !minimap::PoolCodeInstalled())
    {
        platform::Log(L"HUD image: the overlay render is not what the research read; the further players' images stay off");
        return false;
    }
    g_render        = reinterpret_cast<RenderFn>(*render);
    g_submit        = reinterpret_cast<SubmitFn>(*submitFn);
    g_deltaTime     = reinterpret_cast<double*>(*delta);
    g_frameNumber   = reinterpret_cast<std::uint32_t*>(*frame);
    g_acquireSite   = *acquire;
    g_acquireTarget = *acquireFn;
    if (!g_driver.Replace(*slot, *thunk, reinterpret_cast<std::uintptr_t>(&Driver)))
    {
        platform::Log(L"HUD image: the overlay's render delegate is not the game's thunk; the further players' images stay off");
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
              .help  = L"each further player's own HUD image: the overlay rendered first for each further column (on at start; turns "
                       L"hud-targets on)",
              .set   = [](const Value& value) {
                  SetEnabled(std::get<bool>(value));
                  return Outcome{};
              },
              .get = [] { return Value{Enabled()}; }});
    Register({.name  = L"hud-catch-up",
              .shape = Shape::OnOff,
              .help  = L"the further players' HUD passes do not count as the frame's paint, so the game animates and expires "
                       L"what player 1 does not see (damage numbers off player 1's screen); off: they freeze as before (on)",
              .set   = [](const Value& value) {
                  g_catchUp = std::get<bool>(value);
                  platform::Log(L"HUD image: the catch-up of widgets player 1 does not draw {} ({} passes so far)",
                                g_catchUp ? L"on" : L"off", g_catchUpPasses);
                  return Outcome{};
              },
              .get = [] { return Value{g_catchUp}; }});
    Register({.name  = L"hud.image-ms",
              .shape = Shape::Text,
              .help  = L"the overlay render's cost per frame (ms, the last 300 frames): the further columns' passes together, "
                       L"player 1's pass",
              .get   = [] {
                  return Value{std::format(L"further columns {:.3f} ms, player 1 {:.3f} ms", g_lastFurtherMs, g_lastOneMs)};
              }});
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
    const bool acquire = !g_acquireCall.IsInstalled() || (g_gameThread.Here() && g_acquireCall.Restore());
    if (!driver || !acquire)
    {
        platform::Log(L"HUD image: a redirect was changed after us or is off the game thread; left in place (inert: the "
                      L"image is off)");
    }
    g_installed = !(driver && acquire);
}

void Tick(bool splitShown, const Columns& columns) noexcept
{
    g_columns       = columns;
    g_columns.count = std::clamp(columns.count, 1, kMaxColumns);
    if (!g_installed)
    {
        return;
    }
    g_gameThread.Set();
    if (g_acquirePending)
    {
        g_acquirePending = false;  // outside the overlay render here, as InstallFromOwnThread requires
        const bool ok = g_acquireCall.InstallFromOwnThread(g_acquireSite, g_acquireTarget,
                                                           reinterpret_cast<std::uintptr_t>(&Acquire));
        platform::Log(L"HUD image: the overlay's acquire call {}",
                      ok ? L"installed from the game thread" : L"could not be redirected (the further players' images stay off)");
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
    if (g_stockName == 0 && stockName != 0 && !overlay_pass::Further())
    {
        g_stockName = stockName;
        platform::LogDebug(L"HUD image: the further columns' entry names {:#x}.. (the stock {:#x}; number = column + 1, up to {})",
                           overlay_pass::EntryName(stockName, 1), stockName, kMaxColumns);
    }
    return overlay_pass::EntryName(stockName, overlay_pass::Column());
}

}  // namespace lss::hud::second_image
