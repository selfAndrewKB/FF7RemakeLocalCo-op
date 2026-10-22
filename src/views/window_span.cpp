#include "views/window_span.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "views/display_align.h"
#include "views/monitor_row.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace lss::views::window_span {

namespace {

using monitor_row::Rect;
using platform::Read;

// FGenericWindow's virtuals the module calls, by their stock 4.18 slot (the research confirmed the shipped table).
using ReshapeWindowFn     = void (*)(void* self, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
using GetWindowModeFn     = std::int32_t (*)(void* self);
using GetOSWindowHandleFn = void* (*)(void* self);
/// FSystemResolution::RequestResolutionChange(width, height, EWindowMode): sets r.SetRes at console priority.
using RequestResolutionFn = std::uint64_t (*)(std::uint32_t width, std::uint32_t height, std::int32_t mode);

constexpr std::int32_t  kModeFullscreen         = 0;  // EWindowMode::Fullscreen
constexpr std::int32_t  kModeWindowedFullscreen = 1;
constexpr std::int32_t  kModeWindowed           = 2;
constexpr std::uint32_t kHoldTicks              = 30;   // the rect and resolution check, twice a second at 60
constexpr std::uint32_t kStatusTicks            = 300;
constexpr std::uint32_t kRequestTicks           = 180;  // the engine applies a request on its next frame; 3 s is a failure
constexpr std::uint32_t kReshapeSettleTicks     = 2;    // the window's rect is read after the engine's own follow
constexpr std::uint32_t kReshapeRetryTicks      = 60;
constexpr std::uint32_t kLineUpTicks            = 120;  // the desktop read as the row after a move: live 2 ms was too soon
constexpr std::int32_t  kColumnsBuilt           = 2;    // the third column needs the third view and the layout-table write
constexpr std::uint8_t  kLayoutSideBySide       = 1;    // ETwoPlayerSplitScreenType::Vertical
constexpr const wchar_t* kGameMapsSettingsPath  = L"/Script/EngineSettings.Default__GameMapsSettings";

/// The entry's and the leave's steps; each tick moves one on when the engine has done its part.
enum class Step : std::uint8_t
{
    Out,
    LiningUp,   // a row monitor is moved; waiting for the desktop to read as the row (Windows applies it after the call)
    Requested,  // the resolution request is sent; waiting for the engine to apply it
    Reshaped,   // the window is reshaped; waiting for its rect
    In,         // held
    Leaving,    // the window is back and the saved resolution requested; waiting to settle
};

struct Resolution
{
    std::int32_t x = 0, y = 0, mode = 0;
    bool         operator==(const Resolution&) const noexcept = default;
};

bool                g_installed    = false;
std::uintptr_t      g_engineGlobal = 0;        // GEngine, live
std::uintptr_t      g_vtable       = 0;        // FWindowsWindow's vtable, live
RequestResolutionFn g_request      = nullptr;  // live
const std::int32_t* g_systemRes    = nullptr;  // GSystemResolution, live (read only)
bool                g_auto         = true;     // enter on player 2's join
bool                g_autoTried    = false;    // one try per join
bool                g_held         = false;    // the hold: the span with any player count, kept when player 2 leaves
bool                g_holdTried    = false;    // one try per switch-on
bool                g_playerTwoIn  = false;

Step          g_step = Step::Out;
std::uint32_t g_stepTicks = 0;
void*         g_native    = nullptr;  // the native window while in (re-read before each call)
HWND          g_hwnd      = nullptr;
Rect          g_before;               // the client rect (screen coordinates) before the span
std::int32_t  g_beforeMode = -1;
Resolution    g_savedRes;             // GSystemResolution before the span
Rect          g_target;               // the row
Resolution    g_targetRes;
std::int32_t  g_columns    = 0;
std::uint32_t g_ticks      = 0;
std::uint32_t g_reapplies  = 0;
std::uint32_t g_entries    = 0;
std::uint8_t* g_layoutByte = nullptr;  // UGameMapsSettings CDO: TwoPlayerSplitscreenLayout, while the mod changed it
std::uint8_t  g_layoutWas  = 0;

template <class Fn>
Fn Slot(std::size_t index) noexcept
{
    std::uintptr_t entry = 0;
    return Read(reinterpret_cast<const void*>(g_vtable + index * sizeof(void*)), entry) ? reinterpret_cast<Fn>(entry) : nullptr;
}

/// The game's native window: GEngine -> GameViewportWindow (TWeakPtr<SWindow>: the object pointer first) ->
/// SWindow::NativeWindow (TSharedPtr: the object pointer first). Every read guarded; the vtable must be the one
/// the research found, and its GetOSWindowHandle must name a top-level window of this process.
struct NativeWindow
{
    void*        object = nullptr;
    HWND         hwnd   = nullptr;
    std::int32_t mode   = -1;
};

std::optional<NativeWindow> FindNativeWindow(const wchar_t** why) noexcept
{
    std::uintptr_t engine = 0, swindow = 0, native = 0, vtable = 0;
    if (!Read(reinterpret_cast<const void*>(g_engineGlobal), engine) || engine == 0)
    {
        *why = L"GEngine is not set";
        return std::nullopt;
    }
    if (!Read(reinterpret_cast<const void*>(engine + facts::kGameEngineViewportWindow), swindow) || swindow == 0)
    {
        *why = L"UGameEngine::GameViewportWindow is empty";
        return std::nullopt;
    }
    if (!Read(reinterpret_cast<const void*>(swindow + facts::kSWindowNativeWindow), native) || native == 0)
    {
        *why = L"SWindow::NativeWindow is empty";
        return std::nullopt;
    }
    if (!Read(reinterpret_cast<const void*>(native), vtable) || vtable != g_vtable)
    {
        *why = L"the native window's vtable is not FWindowsWindow's";
        return std::nullopt;
    }
    NativeWindow window;
    window.object = reinterpret_cast<void*>(native);
    void* handle  = nullptr;
    if (!platform::CallGuarded(Slot<GetOSWindowHandleFn>(facts::kWindowsWindowSlotGetOSWindowHandle), handle, window.object))
    {
        *why = L"GetOSWindowHandle faulted";
        return std::nullopt;
    }
    window.hwnd = static_cast<HWND>(handle);
    DWORD owner = 0;
    GetWindowThreadProcessId(window.hwnd, &owner);
    if (!IsWindow(window.hwnd) || owner != GetCurrentProcessId() || GetParent(window.hwnd) != nullptr)
    {
        *why = L"GetOSWindowHandle is not a top-level window of this process";
        return std::nullopt;
    }
    if (!platform::CallGuarded(Slot<GetWindowModeFn>(facts::kWindowsWindowSlotGetWindowMode), window.mode, window.object))
    {
        *why = L"GetWindowMode faulted";
        return std::nullopt;
    }
    return window;
}

/// The window's client area in screen coordinates: what ReshapeWindow takes (it adds the OS border itself).
Rect ClientRect(HWND hwnd) noexcept
{
    RECT  client{};
    POINT origin{};
    GetClientRect(hwnd, &client);
    ClientToScreen(hwnd, &origin);
    return {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
}

Rect WindowRect(HWND hwnd) noexcept
{
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    return {rect.left, rect.top, rect.right, rect.bottom};
}

std::wstring RectText(const Rect& r)
{
    return std::format(L"({}, {}, {}, {}) {}x{}", r.left, r.top, r.right, r.bottom, r.Width(), r.Height());
}

const wchar_t* ModeName(std::int32_t mode) noexcept
{
    switch (mode)
    {
        case kModeFullscreen: return L"Fullscreen (exclusive)";
        case kModeWindowedFullscreen: return L"Borderless";
        case kModeWindowed: return L"Windowed";
        default: return L"unknown";
    }
}

Resolution ReadResolution() noexcept
{
    Resolution res;
    std::int32_t values[3]{};
    if (platform::CopyGuarded(values, g_systemRes, sizeof(values)))
    {
        res = {values[0], values[1], values[2]};
    }
    return res;
}

std::wstring ResolutionText(const Resolution& res)
{
    return std::format(L"{}x{} {}", res.x, res.y, ModeName(res.mode));
}

/// The game viewport's size (FViewport SizeX/SizeY), or 0x0 if unread.
std::wstring ViewportText() noexcept
{
    std::uintptr_t engine = 0, client = 0, viewport = 0;
    std::int32_t   size[2]{};
    if (Read(reinterpret_cast<const void*>(g_engineGlobal), engine) && engine != 0 &&
        Read(reinterpret_cast<const void*>(engine + facts::kEngineGameViewport), client) && client != 0 &&
        Read(reinterpret_cast<const void*>(client + facts::kViewportClientViewport), viewport) && viewport != 0 &&
        platform::CopyGuarded(size, reinterpret_cast<const void*>(viewport + facts::kViewportSizeX), sizeof(size)))
    {
        return std::format(L"{}x{}", size[0], size[1]);
    }
    return L"unread";
}

/// Every monitor with its effective DPI, its GDI device name (for a move) and the index of the game's.
struct Monitors
{
    std::vector<monitor_row::Monitor> list;
    std::vector<std::wstring>         devices;
    HMONITOR                          own    = nullptr;  // the game window's monitor, to find
    std::size_t                       gameAt = 0;        // its index; list.size() when it is not listed
};

BOOL CALLBACK CollectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM param) noexcept
{
    auto*         out = reinterpret_cast<Monitors*>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info))
    {
        return TRUE;
    }
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const auto getDpi = reinterpret_cast<GetDpiForMonitorFn>(
        reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor")));
    if (getDpi != nullptr)
    {
        getDpi(handle, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY);
    }
    if (handle == out->own)
    {
        out->gameAt = out->list.size();
    }
    out->list.push_back({{info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom}, dpiX});
    out->devices.emplace_back(info.szDevice);
    return TRUE;
}

/// The desktop now, with the game window's monitor marked.
Monitors Collect(HWND game) noexcept
{
    Monitors monitors;
    monitors.own    = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
    monitors.gameAt = SIZE_MAX;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.gameAt == SIZE_MAX)
    {
        monitors.gameAt = monitors.list.size();
    }
    return monitors;
}

/// The choice's moves as the display API takes them: each monitor by the device name its rect was listed with.
std::vector<display_align::Move> MovesFor(const monitor_row::Choice& choice, const Monitors& monitors)
{
    std::vector<display_align::Move> moves;
    for (const auto& move : choice.moves)
    {
        for (std::size_t i = 0; i < monitors.list.size(); ++i)
        {
            if (monitors.list[i].rect == move.from)
            {
                moves.push_back({monitors.devices[i], move.toTop});
            }
        }
    }
    return moves;
}

bool Reshape(void* native, const Rect& rect) noexcept
{
    return platform::CallGuarded(Slot<ReshapeWindowFn>(facts::kWindowsWindowSlotReshapeWindow), native, rect.left, rect.top,
                                 rect.Width(), rect.Height());
}

bool Request(const Resolution& res) noexcept
{
    std::uint64_t result = 0;
    return platform::CallGuarded(g_request, result, static_cast<std::uint32_t>(res.x), static_cast<std::uint32_t>(res.y),
                                 res.mode);
}

/// The engine's two-player layout byte on the settings' default object: side by side while the span is in.
void SetLayout(bool sideBySide) noexcept
{
    if (sideBySide)
    {
        auto* settings = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(nullptr, nullptr, kGameMapsSettingsPath);
        auto* byte     = settings == nullptr ? nullptr
                                             : reinterpret_cast<std::uint8_t*>(settings) + facts::kGameMapsTwoPlayerLayout;
        std::uint8_t was = 0;
        if (byte == nullptr || !Read(byte, was))
        {
            platform::LogWarning(L"window span: the game's layout settings were not found; the halves stay top and bottom");
            return;
        }
        const std::uint8_t want = kLayoutSideBySide;
        if (was != want && !platform::WriteGuarded(byte, &want, 1))
        {
            platform::LogWarning(L"window span: the two-player layout byte could not be written; the halves stay top and bottom");
            return;
        }
        g_layoutByte = byte;
        g_layoutWas  = was;
        platform::Log(L"window span: the engine's two-player layout set to side by side (was {})", was);
    }
    else if (g_layoutByte != nullptr)
    {
        const std::uint8_t now = g_layoutWas;
        platform::WriteGuarded(g_layoutByte, &now, 1);
        platform::Log(L"window span: the engine's two-player layout put back ({})", g_layoutWas);
        g_layoutByte = nullptr;
    }
}

void Forget() noexcept
{
    g_step      = Step::Out;
    g_stepTicks = 0;
    g_native    = nullptr;
    g_hwnd      = nullptr;
    g_columns   = 0;
    g_ticks     = 0;
}

/// The entry's first step again (the hold found the resolution changed, or the request was lost).
void RequestAgain(const wchar_t* why) noexcept
{
    ++g_reapplies;
    const bool sent = Request(g_targetRes);
    platform::Log(L"window span: {}; the row's resolution requested again{} (re-apply {})", why,
                  sent ? L"" : L" (the request faulted)", g_reapplies);
    g_step      = Step::Requested;
    g_stepTicks = 0;
}

void ReshapeAgain(const wchar_t* why) noexcept
{
    ++g_reapplies;
    const wchar_t* reason = L"";
    const auto     window = FindNativeWindow(&reason);
    const bool     done   = window && Reshape(window->object, g_target);
    platform::Log(L"window span: {}; the window reshaped again to the row{} (re-apply {})", why,
                  done ? L"" : std::format(L" (could NOT: {})", window ? L"ReshapeWindow faulted" : reason).c_str(), g_reapplies);
    g_step      = Step::Reshaped;
    g_stepTicks = 0;
}

/// The entry proper, with the row's monitors level: the layout byte, the resolution request, the globals, the log.
bool Proceed(const NativeWindow& window, const monitor_row::Choice& choice) noexcept
{
    const Resolution before = g_savedRes;
    const Rect       client = g_before;
    g_targetRes             = {choice.span.Width(), choice.span.Height(), kModeWindowedFullscreen};
    SetLayout(true);
    if (!Request(g_targetRes))
    {
        platform::LogWarning(L"window span: the resolution request faulted; nothing changed");
        SetLayout(false);
        display_align::Restore();
        Forget();
        return false;
    }
    g_step      = Step::Requested;
    g_stepTicks = 0;
    g_target    = choice.span;
    ++g_entries;
    platform::Log(L"window span: entering: {} columns over the row of {} equal monitor(s){}{}; the game's resolution request "
                  L"sent for {} (was {}; the window's client area {} in {} mode, the viewport {}); the reshape to {} follows "
                  L"once the engine applies it",
                  g_columns, choice.rowMonitors, choice.gamesRow ? L"" : L" (not the game's own monitor's row)",
                  display_align::Moved() ? std::format(L" (lined up: {})", display_align::Reading()).c_str() : L"",
                  ResolutionText(g_targetRes), ResolutionText(before), RectText(client), ModeName(window.mode), ViewportText(),
                  RectText(choice.span));
    return true;
}

const wchar_t* StepName(Step step) noexcept
{
    switch (step)
    {
        case Step::Out: return L"out";
        case Step::LiningUp: return L"lining up the row";
        case Step::Requested: return L"resolution requested";
        case Step::Reshaped: return L"window reshaped";
        case Step::In: return L"in";
        case Step::Leaving: return L"leaving";
    }
    return L"?";
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto engineGlobal = image.Rebase(facts::kGEngine, sizeof(std::uintptr_t));
    const auto vtable       = image.Rebase(facts::kWindowsWindowVTable, 30 * sizeof(std::uintptr_t));
    const auto systemRes    = image.Rebase(facts::kSystemResolution, 3 * sizeof(std::int32_t));
    const auto reshape      = platform::FindFunction(image, facts::kWindowsWindowReshapeWindow);
    const auto mode         = platform::FindFunction(image, facts::kWindowsWindowGetWindowMode);
    const auto handle       = platform::FindFunction(image, facts::kWindowsWindowGetOSWindowHandle);
    const auto request      = platform::FindFunction(image, facts::kRequestResolutionChange);
    const auto slotReshape  = image.ReadPointer(facts::kWindowsWindowVTable + facts::kWindowsWindowSlotReshapeWindow * 8);
    const auto slotMode     = image.ReadPointer(facts::kWindowsWindowVTable + facts::kWindowsWindowSlotGetWindowMode * 8);
    const auto slotHandle   = image.ReadPointer(facts::kWindowsWindowVTable + facts::kWindowsWindowSlotGetOSWindowHandle * 8);
    const bool slotsHold    = reshape && mode && handle && slotReshape && slotMode && slotHandle && *slotReshape == *reshape &&
                           *slotMode == *mode && *slotHandle == *handle;
    const bool witness = image.BytesMatch(facts::kSetWindowTitleExec.address, facts::kSetWindowTitleExec.entryBytes);
    if (!engineGlobal || !vtable || !systemRes || !request || !slotsHold || !witness)
    {
        platform::LogWarning(L"window span: the game window's code is not what the research read (GEngine {}, vtable {}, "
                             L"slots {}, resolution request {}, witness {}); the span is unavailable",
                             engineGlobal ? L"found" : L"missing", vtable ? L"found" : L"missing", slotsHold ? L"hold" : L"differ",
                             request ? L"found" : L"missing", witness ? L"matches" : L"differs");
        return false;
    }
    g_engineGlobal = *engineGlobal;
    g_vtable       = *vtable;
    g_systemRes    = reinterpret_cast<const std::int32_t*>(*systemRes);
    g_request      = reinterpret_cast<RequestResolutionFn>(*request);
    g_installed    = true;
    control::Register({.name  = L"window-span",
                       .shape = control::Shape::OnOff,
                       .help  = L"the game window across a row of equal monitors, one full view per player (on: enters "
                                L"now and on each join; off: leaves and stays single; Ctrl+F2 toggles)",
                       .set   = [](const control::Value& v) {
                           g_auto = std::get<bool>(v);
                           if (g_auto)
                           {
                               g_autoTried = true;
                               if (g_step == Step::Out && !Enter(g_playerTwoIn ? kColumnsBuilt : 1))
                               {
                                   return control::Outcome{false, L"the span could not be entered (see the log)"};
                               }
                               return control::Outcome{};
                           }
                           Leave(L"switched off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_auto}; }});
    control::Register({.name  = L"window-span.hold",
                       .shape = control::Shape::OnOff,
                       .help  = L"on: the window spans the row now with any player count and stays spanned when player 2 "
                                L"leaves (loading screens, the main menu, movies); off: back to following player 2 (leaves "
                                L"now if player 2 is out)",
                       .set   = [](const control::Value& v) {
                           g_held = std::get<bool>(v);
                           if (g_held)
                           {
                               g_holdTried = true;
                               if (g_step == Step::Out && !Enter(kColumnsBuilt))
                               {
                                   return control::Outcome{false, L"the span could not be entered (see the log)"};
                               }
                               platform::Log(L"window span: held (with any player count, kept when player 2 leaves)");
                               return control::Outcome{};
                           }
                           platform::Log(L"window span: the hold is off; the span follows player 2 again");
                           if (!g_playerTwoIn)
                           {
                               Leave(L"the hold switched off with player 2 out");
                           }
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_held}; }});
    control::Register({.name = L"window-span.state", .shape = control::Shape::Text,
                       .help = L"the span's step, the window and the resolution",
                       .get  = [] { return control::Value{Reading()}; }});
    const auto res = ReadResolution();
    platform::Log(L"window span: ready (on by default: with player 2 in, the window spans a row of equal monitors through "
                  L"the game's resolution request and the engine's window reshape; Ctrl+F2 toggles; window-span.hold on "
                  L"spans with any player count); the game's resolution is {} now",
                  ResolutionText(res));
    return true;
}

bool Installed() noexcept
{
    return g_installed;
}

bool In() noexcept
{
    return g_step != Step::Out && g_step != Step::Leaving;
}

bool Enter(std::int32_t columns) noexcept
{
    if (!g_installed)
    {
        platform::Log(L"window span: not installed");
        return false;
    }
    if (g_step != Step::Out)
    {
        platform::Log(L"window span: already {} ({} columns)", StepName(g_step), g_columns);
        return false;
    }
    if (columns < 2)
    {
        platform::Log(L"window span: {} local player(s); the span needs at least two (one column per seated player)", columns);
        return false;
    }
    if (columns > kColumnsBuilt)
    {
        platform::Log(L"window span: {} columns asked for; only two are built (the third needs the third view)", columns);
        return false;
    }
    const wchar_t* why    = L"";
    const auto     window = FindNativeWindow(&why);
    if (!window)
    {
        platform::Log(L"window span: the game window could not be reached: {}", why);
        return false;
    }
    if (window->mode == kModeFullscreen)
    {
        platform::Log(L"window span: the game is in exclusive fullscreen; set it to Borderless or Windowed first");
        return false;
    }

    Monitors monitors = Collect(window->hwnd);
    auto     choice   = monitor_row::Choose(monitors.list, monitors.gameAt, columns);
    if (!choice.ok)
    {
        platform::Log(L"window span: refused: {} ({} monitor(s) listed; the mod lines up a row's tops itself within {} px "
                      L"and changes no display mode or scaling: arrange equal monitors in a row at 100 % scaling)",
                      choice.why, monitors.list.size(), monitor_row::kAlignTolerance);
        return false;
    }
    g_native     = window->object;
    g_hwnd       = window->hwnd;
    g_before     = ClientRect(window->hwnd);
    g_beforeMode = window->mode;
    g_savedRes   = ReadResolution();
    g_columns    = columns;
    g_ticks      = 0;
    g_reapplies  = 0;
    if (!choice.moves.empty())
    {
        // The row's monitors a few pixels off its top are lined up first. Windows applies the move after the call
        // returns (live, the desktop still read the old layout 2 ms later), so the request waits for a tick on
        // which the desktop reads as the row with nothing left to move.
        const auto moves = MovesFor(choice, monitors);
        if (moves.size() != choice.moves.size() || !display_align::Apply(moves))
        {
            platform::Log(L"window span: refused: {} monitor(s) of the row could not be lined up with its top (see above)",
                          choice.moves.size());
            Forget();
            return false;
        }
        g_step      = Step::LiningUp;
        g_stepTicks = 0;
        platform::Log(L"window span: lining up the row ({}); the entry follows once the desktop reads as the row",
                      display_align::Reading());
        return true;
    }
    return Proceed(*window, choice);
}

void Leave(const wchar_t* why) noexcept
{
    if (g_step == Step::Out || g_step == Step::Leaving)
    {
        return;
    }
    const wchar_t* reason = L"";
    const auto     window = FindNativeWindow(&reason);
    const bool     back   = window && Reshape(window->object, g_before);
    const bool     sent   = Request(g_savedRes);
    SetLayout(false);
    platform::Log(L"window span: {}: the window {} {}{}; the saved resolution {} requested{}; re-applied {} time(s) while in",
                  why, back ? L"put back to" : L"could NOT be put back to", RectText(g_before),
                  window ? L"" : std::format(L" ({})", reason).c_str(), ResolutionText(g_savedRes),
                  sent ? L"" : L" (the request faulted)", g_reapplies);
    g_step      = Step::Leaving;
    g_stepTicks = 0;
}

void Toggle() noexcept
{
    if (In())
    {
        g_auto = false;
        Leave(L"Ctrl+F2");
    }
    else
    {
        g_auto      = true;
        g_autoTried = true;
        if (!g_playerTwoIn && !g_held)
        {
            platform::Log(L"window span: Ctrl+F2 with player 2 out: the span enters on the join (window-span.hold on "
                          L"spans now)");
            return;
        }
        Enter(kColumnsBuilt);
    }
}

void Tick(bool playerTwoIn) noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_playerTwoIn = playerTwoIn;
    if (g_step == Step::Out)
    {
        if (!playerTwoIn)
        {
            g_autoTried = false;
        }
        if (g_held && !g_holdTried)
        {
            g_holdTried = true;
            Enter(kColumnsBuilt);
        }
        else if (playerTwoIn && g_auto && !g_autoTried)
        {
            g_autoTried = true;
            Enter(kColumnsBuilt);
        }
        return;
    }
    if (!playerTwoIn && !g_held && g_step != Step::Leaving)
    {
        Leave(L"player 2 is out");
    }
    ++g_ticks;
    ++g_stepTicks;
    switch (g_step)
    {
        case Step::LiningUp:
        {
            const wchar_t* reason = L"";
            const auto     window = FindNativeWindow(&reason);
            if (!window)
            {
                platform::LogWarning(L"window span: the game window could not be reached during the line-up ({}); the "
                                     L"monitors are put back",
                                     reason);
                display_align::Restore();
                Forget();
                break;
            }
            const Monitors monitors = Collect(window->hwnd);
            const auto     choice   = monitor_row::Choose(monitors.list, monitors.gameAt, g_columns);
            if (choice.ok && choice.moves.empty())
            {
                platform::Log(L"window span: the desktop reads as the row after {} tick(s)", g_stepTicks);
                Proceed(*window, choice);
            }
            else if (g_stepTicks > kLineUpTicks)
            {
                platform::LogWarning(L"window span: after the line-up the desktop still does not read as the row ({}) after "
                                     L"{} ticks; the monitors are put back",
                                     choice.ok ? std::format(L"{} move(s) still listed", choice.moves.size()) : choice.why,
                                     g_stepTicks);
                display_align::Restore();
                Forget();
            }
            break;
        }
        case Step::Requested:
        {
            const Resolution res = ReadResolution();
            if (res == g_targetRes)
            {
                const wchar_t* reason = L"";
                const auto     window = FindNativeWindow(&reason);
                const Rect     seen   = window ? ClientRect(window->hwnd) : Rect{};
                const bool     done   = window && Reshape(window->object, g_target);
                platform::Log(L"window span: the engine applied {} after {} tick(s) (the viewport {}, the window's client area {}); "
                              L"the window {} to the row {}",
                              ResolutionText(res), g_stepTicks, ViewportText(), RectText(seen),
                              done ? L"reshaped" : std::format(L"could NOT be reshaped ({})", window ? L"ReshapeWindow faulted" : reason).c_str(),
                              RectText(g_target));
                g_step      = Step::Reshaped;
                g_stepTicks = 0;
            }
            else if (g_stepTicks > kRequestTicks)
            {
                platform::LogWarning(L"window span: the resolution is still {} after {} ticks (asked for {}); giving up",
                                     ResolutionText(res), g_stepTicks, ResolutionText(g_targetRes));
                Leave(L"the resolution request was not applied");
            }
            break;
        }
        case Step::Reshaped:
        {
            if (g_stepTicks < kReshapeSettleTicks)
            {
                break;
            }
            const Rect seen = IsWindow(g_hwnd) ? ClientRect(g_hwnd) : Rect{};
            if (seen == g_target)
            {
                platform::Log(L"window span: in: {} columns, the window's client area {}, the viewport {}, the resolution {}",
                              g_columns, RectText(seen), ViewportText(), ResolutionText(ReadResolution()));
                g_step      = Step::In;
                g_stepTicks = 0;
            }
            else if (g_stepTicks > kReshapeRetryTicks)
            {
                ReshapeAgain(std::format(L"the window's client area is {}, not the row", RectText(seen)).c_str());
            }
            break;
        }
        case Step::In:
        {
            if (g_ticks % kHoldTicks == 0)
            {
                const Resolution res  = ReadResolution();
                const Rect       seen = IsWindow(g_hwnd) ? ClientRect(g_hwnd) : Rect{};
                if (res != g_targetRes)
                {
                    RequestAgain(std::format(L"the resolution is {}, not the row's (the window's client area {})",
                                             ResolutionText(res), RectText(seen))
                                     .c_str());
                }
                else if (seen != g_target)
                {
                    ReshapeAgain(std::format(L"the window's client area is {}, not the row", RectText(seen)).c_str());
                }
            }
            if (g_ticks % kStatusTicks == 0 && platform::Enabled(platform::Level::Debug))
            {
                platform::LogDebug(L"window span: {} columns, the window's client area {}, the viewport {}, the resolution {}, "
                                   L"re-applies {}",
                                   g_columns, RectText(ClientRect(g_hwnd)), ViewportText(), ResolutionText(ReadResolution()),
                                   g_reapplies);
            }
            break;
        }
        case Step::Leaving:
        {
            const Resolution res = ReadResolution();
            if (res == g_savedRes || g_stepTicks > kRequestTicks)
            {
                const wchar_t* reason = L"";
                const auto     window = FindNativeWindow(&reason);
                Rect           seen   = window ? ClientRect(window->hwnd) : Rect{};
                bool           moved  = false;
                if (window && seen != g_before)
                {
                    moved = Reshape(window->object, g_before);  // a Windowed window was re-centred by the engine
                    seen  = ClientRect(window->hwnd);
                }
                platform::Log(L"window span: out after {} tick(s): the resolution {}{}, the window's client area {}{} (was {} "
                              L"in {} mode), the viewport {}",
                              g_stepTicks, ResolutionText(res), res == g_savedRes ? L"" : L" (NOT the saved one)", RectText(seen),
                              moved ? L" after a last reshape" : L"", RectText(g_before), ModeName(g_beforeMode), ViewportText());
                display_align::Restore();  // after the window is back: the engine sees one desktop change, not two at once
                Forget();
            }
            break;
        }
        case Step::Out:
            break;
    }
}

std::wstring Reading() noexcept
{
    if (!g_installed)
    {
        return L"unavailable";
    }
    if (g_step == Step::Out)
    {
        return std::format(L"out; auto {}; hold {}; entries {}; the resolution {}", g_auto ? L"on" : L"off",
                           g_held ? L"on" : L"off", g_entries, ResolutionText(ReadResolution()));
    }
    return std::format(L"{} ({} columns, row {}; hold {}; player 2 {}); the window's client area {}; the resolution {}; "
                       L"the viewport {}; re-applies {}{}",
                       StepName(g_step), g_columns, RectText(g_target), g_held ? L"on" : L"off", g_playerTwoIn ? L"in" : L"out",
                       RectText(IsWindow(g_hwnd) ? ClientRect(g_hwnd) : Rect{}), ResolutionText(ReadResolution()), ViewportText(),
                       g_reapplies, display_align::Moved() ? std::format(L"; monitors lined up: {}", display_align::Reading()) : L"");
}

bool Held() noexcept
{
    return g_held;
}

}  // namespace lss::views::window_span
