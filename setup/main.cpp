// The setup app (its file is named in views/display_move.h): a small window, run once, that says which monitors the game spans. It shows the desktop as
// Windows arranges it, lets the player click the monitors of the row in the order wanted, and writes LocalSplitScreen.ini beside
// itself (or at the path given as its one argument). The game is then started from Steam as always: no launcher. Every name, word
// and rule comes from the mod's own table and row code (platform/settings.h, views/monitor_picks), so nothing here can drift from
// what the game does. Nothing is saved while the game runs: the mod keeps the file's values in memory and its Co-op page would
// write them over the app's.
//
// The window is one surface the app draws itself (setup/view), laid out by setup/setup_rules.h: no child controls. The mouse and
// the keyboard both reach every part: Tab and the arrow keys move, Left and Right change an option, Space or Enter presses,
// Enter alone saves, Escape closes.
//
// The same exe is the mod's guard (setup/guard).

#include "platform/settings_write.h"
#include "setup/app.h"
#include "setup/guard.h"
#include "setup/setup_rules.h"
#include "setup/theme.h"
#include "setup/tips.h"
#include "setup/view.h"
#include "setup_version.h"
#include "views/display_move.h"

#include <CommCtrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <filesystem>
#include <format>
#include <string>

#define LSS_WIDE_(text) L##text
#define LSS_WIDE(text)  LSS_WIDE_(text)

namespace {

using namespace lss::setup::app;
namespace rules = lss::setup::rules;
namespace tips  = lss::setup::tips;
namespace view  = lss::setup::view;

constexpr const wchar_t* kTitle     = L"Multi Monitor Setup";
constexpr const wchar_t* kModFolder = L"dlls";  // stands beside the settings file where the mod reads it
constexpr UINT_PTR       kGameTimer = 1;
constexpr int            kWidth     = 880;  // the client area the window opens with, at 96 dpi
constexpr int            kHeight    = 560;

/// The part under the mouse, from a mouse message's point.
rules::Hit HitAt(LPARAM param)
{
    return lss::setup::app::HitAt(static_cast<short>(LOWORD(param)), static_cast<short>(HIWORD(param)));
}

/// The title bar in the window's own colours, where Windows offers it.
void DarkTitleBar(HWND window)
{
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF bar = lss::setup::theme::kWindow.top;
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &bar, sizeof(bar));
}

SIZE WindowSizeFor(int clientWidth, int clientHeight, UINT dpi)
{
    RECT rect{0, 0, MulDiv(clientWidth, static_cast<int>(dpi), 96), MulDiv(clientHeight, static_cast<int>(dpi), 96)};
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    return {rect.right - rect.left, rect.bottom - rect.top};
}

/// One paint of the window, ended however the drawing leaves.
struct Paint
{
    HWND        window;
    PAINTSTRUCT paint{};
    HDC         dc;

    explicit Paint(HWND target) noexcept : window(target), dc(BeginPaint(target, &paint)) {}
    ~Paint() { EndPaint(window, &paint); }
    Paint(const Paint&)            = delete;
    Paint& operator=(const Paint&) = delete;
};

LRESULT Handle(HWND window, UINT message, WPARAM wide, LPARAM param)
{
    switch (message)
    {
        case WM_CREATE:
            g_app.window = window;
            g_app.dpi    = GetDpiForWindow(window);
            g_app.fonts  = view::MakeFonts(g_app.dpi);
            DarkTitleBar(window);
            tips::Make(window, kTipCount, g_app.fonts.minor, g_app.dpi);
            g_app.gameRunning = GameIsRunning();
            SetTimer(window, kGameTimer, 2000, nullptr);
            Refresh();
            return 0;
        case WM_TIMER:
            if (wide == kGameTimer)
            {
                const bool running = GameIsRunning();
                if (running != g_app.gameRunning)
                {
                    g_app.gameRunning = running;
                    ReadMonitors();  // the game's end may have put monitors back
                    Refresh();
                }
                else if (const bool restore = !running && !displays::IsSavedLayout(); restore != g_app.canRestore)
                {
                    Refresh();
                }
            }
            return 0;
        case WM_DPICHANGED:
        {
            const RECT*       size  = reinterpret_cast<const RECT*>(param);
            const view::Fonts fresh = view::MakeFonts(HIWORD(wide));
            view::Fonts       old   = g_app.fonts;
            g_app.fonts             = fresh;
            g_app.dpi               = HIWORD(wide);
            tips::Style(g_app.fonts.minor, g_app.dpi);  // the hover text takes its new font before the one it holds goes
            view::Free(old);
            SetWindowPos(window, nullptr, size->left, size->top, size->right - size->left, size->bottom - size->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Refresh();
            return 0;
        }
        case WM_DISPLAYCHANGE:
            if (g_app.focus.part == rules::Part::Tile)
            {
                g_app.focus = {};  // the list is read anew: the same place in it may be another monitor
            }
            ReadMonitors();
            Refresh();
            return 0;
        case WM_SIZE:
            PlaceTips();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO:
        {
            const SIZE least = WindowSizeFor(rules::kMinWidth, rules::kMinHeight, GetDpiForWindow(window));
            reinterpret_cast<MINMAXINFO*>(param)->ptMinTrackSize = {least.cx, least.cy};
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_app.tracking)
            {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
                g_app.tracking = TrackMouseEvent(&track) != FALSE;
            }
            if (const rules::Hit hit = HitAt(param); hit != g_app.hover)
            {
                g_app.hover = hit;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_app.tracking = false;
            g_app.hover    = {};
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            SetCapture(window);
            g_app.pressed   = HitAt(param);
            g_app.hover     = g_app.pressed;
            g_app.showFocus = false;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
        {
            const rules::Hit held = g_app.pressed;  // read first: giving the mouse up clears it
            if (GetCapture() == window)
            {
                ReleaseCapture();
            }
            g_app.pressed = {};
            InvalidateRect(window, nullptr, FALSE);
            // A press counts when it ends on the part it began on.
            if (const rules::Hit hit = HitAt(param); hit == held && hit.part != rules::Part::None)
            {
                Press(hit);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            g_app.pressed = {};
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(param) == HTCLIENT && g_app.hover.part != rules::Part::None)
            {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));  // IDC_HAND, as the wide call takes it
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
            OnKey(wide, (param & (1 << 30)) != 0);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // the paint covers everything
        case WM_PAINT:
        {
            const Paint paint(window);
            RECT        client{};
            GetClientRect(window, &client);
            view::Draw(paint.dc, client, SceneNow(), g_app.fonts, g_app.dpi);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(window, kGameTimer);
            tips::Destroy();  // before the font it holds
            view::Free(g_app.fonts);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wide, param);
}

/// Nothing thrown may pass into Windows' own code. A message that ran out of memory is dropped and the window lives on; a
/// window that cannot even be made says so by failing its creation.
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wide, LPARAM param) noexcept
try
{
    return Handle(window, message, wide, param);
}
catch (...)
{
    return message == WM_CREATE ? -1 : 0;
}

/// The settings file: the path given, else beside this exe, where the mod keeps it.
std::wstring FindFile()
{
    int        count = 0;
    wchar_t**  args  = CommandLineToArgvW(GetCommandLineW(), &count);
    std::wstring given = args != nullptr && count > 1 ? args[1] : L"";
    if (args != nullptr)
    {
        LocalFree(args);
    }
    if (!given.empty())
    {
        return given;
    }
    wchar_t self[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    return (std::filesystem::path(self).parent_path() / settings::kFileName).wstring();
}

/// The settings file stands where the mod reads it: beside the mod's own folder of code.
bool InModFolder(const std::wstring& file)
{
    std::error_code error;
    return std::filesystem::is_directory(std::filesystem::path(file).parent_path() / kModFolder, error);
}

int Fail(const wchar_t* what)
{
    const DWORD        error = GetLastError();  // none when the app's own start failed
    const std::wstring text  = error != 0 ? std::format(L"{} (Windows error {}).", what, error) : std::format(L"{}.", what);
    MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | MB_ICONERROR);
    return 1;
}

int ShowWindowAndRun(HINSTANCE instance, int show)
{
    g_app.path        = FindFile();
    g_app.inModFolder = InModFolder(g_app.path);
    g_app.readings    = settings::ReadFile(g_app.path);
    // A record with no game running: the game died and so did its guard.
    if (!GameIsRunning())
    {
        if (const auto healed = displays::Heal(RecordPath(), 0); healed != displays::Healed::NoRecord)
        {
            Say(std::format(L"From the last game: {}.", displays::Name(healed)), healed == displays::Healed::Failed ? rules::Tone::Problem : rules::Tone::Good);
        }
    }
    g_app.fileWord = g_app.readings[IndexOf(settings::kMonitors)].word;
    ReadMonitors();

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW type{};
    type.cbSize        = sizeof(type);
    type.style         = CS_HREDRAW | CS_VREDRAW;
    type.lpfnWndProc   = WindowProc;
    type.hInstance     = instance;
    type.hIcon         = LoadIconW(instance, MAKEINTRESOURCEW(1));
    type.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW, as the wide call takes it
    type.lpszClassName = LSS_WIDE(LSS_SETUP_NAME);
    if (RegisterClassExW(&type) == 0)
    {
        return Fail(L"The window could not be set up");
    }
    const SIZE size   = WindowSizeFor(kWidth, kHeight, GetDpiForSystem());
    const HWND window = CreateWindowExW(0, type.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, size.cx, size.cy,
                                        nullptr, nullptr, instance, nullptr);
    if (window == nullptr)
    {
        return Fail(L"The window could not be opened");
    }
    ShowWindow(window, show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    // The guard has no window and nobody watching: a fault there ends it quietly, its exit code saying the monitors may not
    // have been put back.
    try
    {
        if (int guardExit = 0; lss::setup::guard::Run(guardExit))
        {
            return guardExit;
        }
    }
    catch (...)
    {
        return 1;
    }
    try
    {
        return ShowWindowAndRun(instance, show);
    }
    catch (...)
    {
        MessageBoxW(nullptr, L"The app ran into a problem it could not recover from and has to close.", kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
}
