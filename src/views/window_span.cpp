#include "views/window_span.h"

#include "control/registry.h"
#include "engine_facts.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "players/game_state.h"
#include "views/column_rules.h"
#include "views/display_align.h"
#include "views/monitor_list.h"
#include "views/monitor_row.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <span>
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
constexpr std::uint32_t kLineUpTicks            = 120;  // the desktop read as the row after a move; Windows may apply one after the call returns
constexpr std::uint32_t kLineUpCheckTicks       = 6;    // that read is the display configuration: not every tick
constexpr std::int32_t  kColumnsBuilt           = column_rules::kMaxColumns;  // one per monitor of the row, up to three
constexpr std::uint8_t  kStatePlay              = 6;    // the game's play state (the title is one): the preset's hold waits for it

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
bool                g_auto         = false;    // enter on a further player's join (Ctrl+F2, window-span on, the preset's on)
bool                g_autoTried    = false;    // one try per join
bool                g_held         = false;    // the hold: the span with any player count, kept when the last further player leaves
bool                g_holdTried    = false;    // one try per switch-on (the preset's waits for the game's first steady frame)
bool                g_holdPreset   = false;    // the hold came from the preset: its try waits for the window and the play state
bool                g_furtherPlayerIn = false;
bool                g_retry        = false;    // a span wanted but refused or lost to the desktop: tried again when the desktop changes
std::uint64_t       g_triedShape   = 0;        // the desktop's shape at the last try
std::uint64_t       g_rowShape     = 0;        // the desktop's shape the row stands on
std::uint32_t       g_outTicks     = 0;
bool                g_rowLost      = false;    // the last leave was the desktop's doing: the next entry keeps the window's place from before

/// The preset's word as the live state reads: `off` while neither the hold nor the join rule (Ctrl+F2, `window-span on`) is,
/// else `on`.
std::wstring Preset() noexcept
{
    return g_held || g_auto ? L"on" : L"off";
}

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
std::vector<std::int32_t> g_widths;   // the span's monitors' widths, left to right (one per column), while in
std::uint32_t g_ticks      = 0;
std::uint32_t g_reapplies  = 0;
std::uint32_t g_entries    = 0;

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

/// Every monitor with its effective DPI, its GDI device name (for a move), the index of the game's, and the player's picks
/// found among them.
struct Monitors
{
    std::vector<monitor_row::Monitor> list;
    std::vector<std::wstring>         devices;
    std::size_t                       gameAt = 0;  // the game window's monitor; list.size() when it is not listed
    monitor_row::Picks                picks;
};

/// The desktop's shape: every monitor's rect, hashed, so a monitor coming, going or moving changes it, even one that leaves the
/// desktop's outer bounds as they were. Cheap enough for the twice-a-second check (no device names, no display configuration).
std::uint64_t DesktopShape() noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT rect, LPARAM param) noexcept -> BOOL {
            auto& h = *reinterpret_cast<std::uint64_t*>(param);
            for (const LONG value : {rect->left, rect->top, rect->right, rect->bottom})
            {
                h = (h ^ static_cast<std::uint32_t>(value)) * 1099511628211ull;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hash));
    return hash;
}

/// The desktop now, with the game window's monitor marked and the player's picks found.
Monitors Collect(HWND game) noexcept
try
{
    Monitors           monitors;
    const auto         listed = monitor_list::List();
    const HMONITOR     own    = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
    std::vector<std::wstring> ids, keys;
    monitors.gameAt = listed.size();
    for (std::size_t i = 0; i < listed.size(); ++i)
    {
        monitors.list.push_back(listed[i].monitor);
        monitors.devices.push_back(listed[i].device);
        ids.push_back(listed[i].id);
        keys.push_back(listed[i].key);
        if (listed[i].handle == own)
        {
            monitors.gameAt = i;
        }
    }
    monitors.picks = monitor_row::FindPicks(ids, keys, platform::settings::Of(platform::settings::kMonitors).word);
    return monitors;
}
catch (...)
{
    return {};  // no monitors: the span is refused, with the row's reason in the log
}

/// The player picked monitors in the settings file, whether they are connected or not.
bool HasPicks(const Monitors& monitors) noexcept
{
    return monitors.picks.named > 0;
}

/// The columns the player's connected picks give, up to what is built; 0 when fewer than two are connected.
std::int32_t PickedColumns(const Monitors& monitors) noexcept
{
    const auto found = static_cast<std::int32_t>(monitors.picks.found.size());
    return found < 2 ? 0 : std::min(found, kColumnsBuilt);
}

/// The row for `columns`: the player's picks, moved into a row, whatever they are or wherever they sit; with none picked, a row
/// the desktop has. Picks that are not connected are waited for, never replaced by other monitors.
monitor_row::Choice ChooseRow(const Monitors& monitors, std::int32_t columns)
{
    if (!HasPicks(monitors))
    {
        return monitor_row::Choose(monitors.list, monitors.gameAt, columns);
    }
    const auto& found = monitors.picks.found;
    if (found.size() < 2)
    {
        monitor_row::Choice waiting;
        waiting.why = std::format(L"{} of the {} monitors picked in the setup app are connected; the row waits for at least two",
                                  found.size(), monitors.picks.named);
        return waiting;
    }
    const auto count = std::min(found.size(), static_cast<std::size_t>(columns));
    return monitor_row::Arrange(monitors.list, std::span(found).first(count));
}

/// Every monitor as read: device, rect, DPI, the game's marked.
std::wstring DesktopText(const Monitors& monitors)
{
    std::wstring text;
    for (std::size_t i = 0; i < monitors.list.size(); ++i)
    {
        const auto& m = monitors.list[i];
        text += std::format(L"{}{} {} {} dpi{}", i ? L", " : L"", monitors.devices[i], RectText(m.rect), 
                            i == monitors.gameAt ? L"(the game's)" : L"", m.dpi);
    }
    return text;
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
                moves.push_back({monitors.devices[i], move.toLeft, move.toTop});
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

void Forget() noexcept
{
    g_step      = Step::Out;
    g_stepTicks = 0;
    g_native    = nullptr;
    g_hwnd      = nullptr;
    g_columns   = 0;
    g_widths.clear();
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

/// The entry proper, with the row's monitors level: the resolution request, the globals, the log.
bool Proceed(const NativeWindow& window, const monitor_row::Choice& choice) noexcept
{
    const Resolution before = g_savedRes;
    const Rect       client = g_before;
    g_targetRes             = {choice.span.Width(), choice.span.Height(), kModeWindowedFullscreen};
    if (!Request(g_targetRes))
    {
        platform::LogWarning(L"window span: the resolution request faulted; nothing changed");
        display_align::Restore();
        Forget();
        return false;
    }
    g_step      = Step::Requested;
    g_stepTicks = 0;
    g_rowShape  = DesktopShape();
    g_target    = choice.span;
    g_widths    = choice.widths;
    ++g_entries;
    std::wstring widths;
    for (const std::int32_t width : g_widths)
    {
        widths += std::format(L"{}{}", widths.empty() ? L"" : L" | ", width);
    }
    platform::Log(L"window span: entering: {} columns over the row of {} monitor(s) (the columns {} px wide){}{}; the game's "
                  L"resolution request sent for {} (was {}; the window's client area {} in {} mode, the viewport {}); the reshape "
                  L"to {} follows once the engine applies it",
                  g_columns, choice.rowMonitors, widths, choice.gamesRow ? L"" : L" (not the game's own monitor's row)",
                  display_align::Moved() ? std::format(L" (moved: {})", display_align::Reading()).c_str() : L"",
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
                       .help  = L"the game window across a row of monitors of one height, one full view per player on each (on: enters "
                                L"now and on each join; off: leaves and stays single; Ctrl+F2 toggles)",
                       .set   = [](const control::Value& v) {
                           g_auto = std::get<bool>(v);
                           if (g_auto)
                           {
                               g_autoTried = true;
                               if (g_step == Step::Out && !Enter(g_furtherPlayerIn ? kColumnsBuilt : 1))
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
                       .help  = L"on: the window spans the row now with any player count and stays spanned when the last further player "
                                L"leaves (loading screens, the main menu, movies); off: back to following the further players (leaves "
                                L"now if every one is out)",
                       .set   = [](const control::Value& v) {
                           g_held = std::get<bool>(v);
                           if (g_held)
                           {
                               g_holdTried = true;
                               if (g_step == Step::Out && !Enter(kColumnsBuilt))
                               {
                                   return control::Outcome{false, L"the span could not be entered (see the log)"};
                               }
                               platform::Log(L"window span: held (with any player count, kept when the last further player leaves)");
                               return control::Outcome{};
                           }
                           platform::Log(L"window span: the hold is off; the span follows the further players again");
                           if (!g_furtherPlayerIn)
                           {
                               Leave(L"the hold switched off with every further player out");
                           }
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_held}; }});
    control::Register({.name = L"window-span.state", .shape = control::Shape::Text,
                       .help = L"the span's step, the window and the resolution",
                       .get  = [] { return control::Value{Reading()}; }});
    control::RegisterOption(
        platform::settings::kMultiMonitor,
        [](const std::wstring& word) {
            SetPreset(word, L"the console");
            return control::Outcome{};
        },
        [] { return Preset(); });
    control::Register({.name = platform::settings::kMonitors.key, .shape = control::Shape::Text,
                       .help = platform::settings::kMonitors.help,
                       .get  = [] { return control::Value{platform::settings::Of(platform::settings::kMonitors).word}; }});
    SetPreset(platform::settings::Of(platform::settings::kMultiMonitor).word, platform::settings::Why(platform::settings::kMultiMonitor).c_str());
    display_align::HealEarlierRun();
    const auto res = ReadResolution();
    platform::Log(L"window span: ready (the window spans a row of monitors of one height through the game's resolution request and the "
                  L"engine's window reshape; Ctrl+F2 toggles; window-span.hold on spans with any player count); the game's resolution is {} now",
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
try
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
        platform::Log(L"window span: {} columns asked for; at most {} are built (one per monitor of the row)", columns, kColumnsBuilt);
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
    if (const std::int32_t picked = PickedColumns(monitors); picked > 0)
    {
        columns = std::min(columns, picked);
    }
    auto choice = ChooseRow(monitors, columns);
    // As many columns as the row has monitors, up to the number asked for: a two-monitor row spans two columns
    // when three are asked (the hold asks for every column built).
    while (!choice.ok && !HasPicks(monitors) && columns > 2 && choice.rowMonitors >= 2)
    {
        const std::int32_t fewer = std::min(columns - 1, choice.rowMonitors);
        platform::Log(L"window span: the longest row has {} monitor(s); {} columns instead of {}", choice.rowMonitors, fewer,
                      columns);
        columns = fewer;
        choice  = monitor_row::Choose(monitors.list, monitors.gameAt, columns);
    }
    if (!choice.ok)
    {
        platform::Log(L"window span: refused: {} ({} monitor(s) listed; the mod changes no display mode or scaling); tried again when the "
                      L"desktop changes",
                      choice.why, monitors.list.size());
        return false;
    }
    g_native     = window->object;
    g_hwnd       = window->hwnd;
    // A row lost and regained is one stretch of play: Windows may have shifted the window in between, and the place to return
    // to is still the one from before the first entry.
    g_before     = g_rowLost ? g_before : ClientRect(window->hwnd);
    g_beforeMode = window->mode;
    g_savedRes   = ReadResolution();
    g_columns    = columns;
    g_ticks      = 0;
    g_reapplies  = 0;
    if (!choice.moves.empty())
    {
        // The row's monitors are moved first. Windows may apply a move after the call returns and says nothing when it
        // ignores one, so the request waits for a tick on which the desktop reads as the row with nothing left to move.
        const auto moves = MovesFor(choice, monitors);
        if (moves.size() != choice.moves.size() || !display_align::Apply(moves))
        {
            platform::Log(L"window span: refused: {} monitor(s) could not be moved to make the row (see above)", choice.moves.size());
            Forget();
            return false;
        }
        g_step      = Step::LiningUp;
        g_stepTicks = 0;
        platform::Log(L"window span: moving monitors to make the row ({}); the entry follows once the desktop reads as the row; the "
                      L"desktop before: {}",
                      display_align::Reading(), DesktopText(monitors));
        return true;
    }
    return Proceed(*window, choice);
}
catch (...)
{
    platform::LogWarning(L"window span: the entry failed for lack of memory");
    return false;
}

void Leave(const wchar_t* why) noexcept
{
    if (g_step == Step::Out || g_step == Step::Leaving)
    {
        return;
    }
    g_rowLost = false;  // the one leave that is the desktop's doing says so after this
    const wchar_t* reason = L"";
    const auto     window = FindNativeWindow(&reason);
    const bool     back   = window && Reshape(window->object, g_before);
    const bool     sent   = Request(g_savedRes);
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
        g_auto    = false;
        g_retry   = false;
        g_rowLost = false;
        Leave(L"Ctrl+F2");
    }
    else
    {
        g_auto      = true;
        g_autoTried = true;
        if (!g_furtherPlayerIn && !g_held)
        {
            platform::Log(L"window span: Ctrl+F2 with every further player out: the span enters on the next join (window-span.hold on "
                          L"spans now)");
            return;
        }
        Enter(kColumnsBuilt);
    }
}

std::int32_t Columns() noexcept
{
    return In() ? g_columns : 0;
}

std::span<const std::int32_t> ColumnWidths() noexcept
{
    return In() ? std::span<const std::int32_t>(g_widths) : std::span<const std::int32_t>();
}

void SetPreset(const std::wstring& preset, const wchar_t* why) noexcept
{
    if (preset == L"on")
    {
        g_auto       = true;
        g_held       = true;
        g_holdPreset = true;
        g_holdTried  = In();  // out: one try at the game's next steady frame (the tick waits for the window and the play
                              // state); in or entering: nothing to try, and no re-entry after a later Ctrl+F2
        platform::Log(L"window span: multi-monitor = on ({}): the span is held {}, with any player count; the mirror player "
                      L"fills every empty column",
                      why, In() ? L"now" : L"from the game's next steady frame");
    }
    else if (preset == L"off")
    {
        g_auto       = false;
        g_held       = false;
        g_holdPreset = false;
        g_retry      = false;
        g_rowLost    = false;
        platform::Log(L"window span: multi-monitor = off ({}): no span until window-span on, window-span.hold on or Ctrl+F2", why);
        if (In())
        {
            Leave(L"multi-monitor off");
        }
    }
    else
    {
        platform::LogWarning(L"window span: multi-monitor = '{}' ({}) is not a word of the table; nothing changes", preset, why);
    }
}

namespace {

/// With the monitors back: a window whose monitor has gone goes to the primary, not off the desktop.
void KeepWindowOnDesktop() noexcept
{
    const RECT before{g_before.left, g_before.top, g_before.right, g_before.bottom};
    if (MonitorFromRect(&before, MONITOR_DEFAULTTONULL) != nullptr)
    {
        return;
    }
    const POINT corner{0, 0};
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(MonitorFromPoint(corner, MONITOR_DEFAULTTOPRIMARY), &info))
    {
        platform::Log(L"window span: the window's old place {} is on no monitor now; it goes to the primary", RectText(g_before));
        g_before = {info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom};
    }
}

/// The monitors were moved: the row is entered once the desktop reads as the row, and given up after a while.
void StepLiningUp() noexcept
try
{
    if (g_stepTicks % kLineUpCheckTicks != 0 && g_stepTicks <= kLineUpTicks)
    {
        return;
    }
    const wchar_t* reason = L"";
    const auto     window = FindNativeWindow(&reason);
    if (!window)
    {
        platform::LogWarning(L"window span: the game window could not be reached during the move ({}); the monitors are put back", reason);
        display_align::Restore();
        Forget();
        return;
    }
    Monitors   monitors = Collect(window->hwnd);
    const auto choice   = ChooseRow(monitors, g_columns);
    if (choice.ok && choice.moves.empty())
    {
        platform::Log(L"window span: the desktop reads as the row after {} tick(s)", g_stepTicks);
        g_retry = !Proceed(*window, choice);
    }
    else if (g_stepTicks > kLineUpTicks)
    {
        platform::LogWarning(L"window span: after the move the desktop still does not read as the row ({}) after {} ticks; the monitors are put "
                             L"back; the desktop now: {}",
                             choice.ok ? std::format(L"{} move(s) still listed", choice.moves.size()) : choice.why, g_stepTicks, DesktopText(monitors));
        display_align::Restore();
        Forget();
        g_retry      = true;
        g_triedShape = DesktopShape();
    }
}
catch (...)
{
    platform::LogWarning(L"window span: the desktop could not be read during the move; the monitors are put back");
    display_align::Restore();
    Forget();
}

/// An entry that, refused, is tried again when the desktop changes.
void TryEnter() noexcept
{
    g_triedShape = DesktopShape();
    g_retry      = !Enter(kColumnsBuilt);
}

/// The desktop's shape changed while the row stands: leaves unless the row still stands as it did, and comes back on its own.
void CheckTheDesktop() noexcept
try
{
    g_rowShape = DesktopShape();
    Monitors         monitors = Collect(g_hwnd);
    const auto       wanted   = PickedColumns(monitors);
    const auto       choice   = ChooseRow(monitors, wanted > 0 ? wanted : g_columns);
    const bool       same     = choice.ok && choice.moves.empty() && choice.span == g_target && (wanted == 0 || wanted == g_columns);
    if (same)
    {
        platform::Log(L"window span: the desktop changed and the row still stands: {}", DesktopText(monitors));
        return;
    }
    platform::Log(L"window span: the desktop changed and the row no longer stands ({}): {}", choice.ok ? L"another row is wanted" : choice.why,
                  DesktopText(monitors));
    g_retry      = true;
    g_triedShape = 0;  // the first try right after the leave
    Leave(L"the desktop changed");
    g_rowLost    = true;
}
catch (...)
{
    platform::LogWarning(L"window span: the changed desktop could not be read");
}

}  // namespace

void Tick(bool furtherPlayerIn) noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_furtherPlayerIn = furtherPlayerIn;
    if (g_step == Step::Out)
    {
        if (!furtherPlayerIn)
        {
            g_autoTried = false;
        }
        if (g_held && !g_holdTried)
        {
            // The preset's hold waits for the game window and the play state (the title is one): a request during
            // the boot would find no window, and one try is all a hold gets.
            const wchar_t* why = L"";
            if (!g_holdPreset || (FindNativeWindow(&why) && players::game_state::Current().state == kStatePlay))
            {
                g_holdTried = true;
                TryEnter();
            }
        }
        else if (furtherPlayerIn && g_auto && !g_autoTried)
        {
            g_autoTried = true;
            TryEnter();
        }
        else if (g_retry && (g_held || (g_auto && furtherPlayerIn)) && ++g_outTicks % kHoldTicks == 0 && DesktopShape() != g_triedShape)
        {
            TryEnter();
        }
        return;
    }
    if (!furtherPlayerIn && !g_held && g_step != Step::Leaving)
    {
        Leave(L"every further player is out");
    }
    ++g_ticks;
    ++g_stepTicks;
    switch (g_step)
    {
        case Step::LiningUp:
            StepLiningUp();
            break;
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
                g_rowLost   = false;
            }
            else if (g_stepTicks > kReshapeRetryTicks)
            {
                ReshapeAgain(std::format(L"the window's client area is {}, not the row", RectText(seen)).c_str());
            }
            break;
        }
        case Step::In:
        {
            if (g_ticks % kHoldTicks == 0 && DesktopShape() != g_rowShape)
            {
                CheckTheDesktop();
                break;
            }
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
                if (display_align::Moved())
                {
                    display_align::Restore();  // the window's rect is read on the desktop it came from
                    break;
                }
                KeepWindowOnDesktop();
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
    return std::format(L"{} ({} columns, row {}; hold {}; further players {}); the window's client area {}; the resolution {}; "
                       L"the viewport {}; re-applies {}{}",
                       StepName(g_step), g_columns, RectText(g_target), g_held ? L"on" : L"off", g_furtherPlayerIn ? L"in" : L"out",
                       RectText(IsWindow(g_hwnd) ? ClientRect(g_hwnd) : Rect{}), ResolutionText(ReadResolution()), ViewportText(),
                       g_reapplies, display_align::Moved() ? std::format(L"; monitors moved: {}", display_align::Reading()) : L"");
}

bool Held() noexcept
{
    return g_held;
}

}  // namespace lss::views::window_span
