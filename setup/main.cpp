// LocalSplitScreenSetup.exe: a small window, run once, that says which monitors the game spans. It shows the desktop as
// Windows arranges it, lets the player click the monitors that make the row, and writes LocalSplitScreen.ini beside itself
// (or at the path given as its one argument). The game is then started from Steam as always: no launcher, no background
// process. Every name, word and rule comes from the mod's own table and row code (platform/settings.h, views/monitor_row),
// so nothing here can drift from what the game does. Nothing is saved while the game runs: the mod keeps the file's values
// in memory and its Co-op page would write them back over the app's.

#include "platform/settings_write.h"
#include "setup/setup_rules.h"
#include "views/monitor_list.h"

#include <CommCtrl.h>
#include <TlHelp32.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "      \
                        "processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

namespace settings = lss::platform::settings;
namespace rules    = lss::setup::rules;
namespace row      = lss::views::monitor_row;
namespace listing  = lss::views::monitor_list;

constexpr const wchar_t* kFileName   = L"LocalSplitScreen.ini";
constexpr const wchar_t* kGameExe    = L"ff7remake_.exe";
constexpr int            kIdMulti    = 101;
constexpr int            kIdOrder    = 102;
constexpr int            kIdSave     = 103;
constexpr int            kIdClose    = 104;
constexpr UINT_PTR       kGameTimer  = 1;

struct App
{
    HWND         window = nullptr, multi = nullptr, order = nullptr, save = nullptr, close = nullptr;
    HWND         multiLabel = nullptr, orderLabel = nullptr, status = nullptr;
    HFONT        font = nullptr, smallFont = nullptr;
    UINT         dpi = 96;
    RECT         picture{};
    std::wstring path;
    std::array<settings::Reading, settings::kOptions.size()> readings;
    std::vector<listing::Listed> monitors;
    std::vector<char>            ticked;  // one per monitor
    bool                         picksChanged = false;  // the player clicked a monitor: the file's own names stay otherwise
    std::int32_t                 namedNotHere = 0;      // monitors the file names that are not connected now
    std::size_t                  primary = 0;
    bool                         gameRunning = false;
    std::wstring                 savedNote;
};
App g_app;

int Px(int value)
{
    return MulDiv(value, static_cast<int>(g_app.dpi), 96);
}

std::vector<row::Monitor> PlainMonitors()
{
    std::vector<row::Monitor> list;
    for (const listing::Listed& m : g_app.monitors)
    {
        list.push_back(m.monitor);
    }
    return list;
}

rules::Look CurrentLook()
{
    return rules::LookAt(PlainMonitors(), g_app.ticked, g_app.primary);
}

std::size_t IndexOf(const settings::Option& option)
{
    return static_cast<std::size_t>(std::find(settings::kOptions.begin(), settings::kOptions.end(), &option) - settings::kOptions.begin());
}

bool GameIsRunning()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found   = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
    {
        found = _wcsicmp(entry.szExeFile, kGameExe) == 0;
    }
    CloseHandle(snapshot);
    return found;
}

/// The monitors now, keeping the ticks of those still there: from the file on the first call, from `was` after.
void ReadMonitors(const std::vector<std::wstring>& wasTicked, bool fromFile)
{
    g_app.monitors = listing::List();
    g_app.ticked.assign(g_app.monitors.size(), 0);
    g_app.primary = 0;
    std::vector<std::wstring> ids;
    for (std::size_t i = 0; i < g_app.monitors.size(); ++i)
    {
        const row::Rect& rect = g_app.monitors[i].monitor.rect;
        if (rect.left == 0 && rect.top == 0)
        {
            g_app.primary = i;
        }
        ids.push_back(g_app.monitors[i].id);
    }
    if (fromFile)
    {
        auto             plain = PlainMonitors();
        const row::Named named = row::MarkNamed(plain, ids, g_app.readings[IndexOf(settings::kMonitors)].word);
        g_app.namedNotHere     = named.named - named.present;
        for (std::size_t i = 0; named.present >= 2 && i < plain.size(); ++i)
        {
            g_app.ticked[i] = plain[i].joins ? 1 : 0;
        }
        return;
    }
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        g_app.ticked[i] = std::find(wasTicked.begin(), wasTicked.end(), ids[i]) != wasTicked.end() ? 1 : 0;
    }
}

std::vector<std::wstring> TickedIds()
{
    // Left to right: the word reads the same however Windows lists them.
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < g_app.ticked.size(); ++i)
    {
        if (g_app.ticked[i] != 0)
        {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [](std::size_t a, std::size_t b) {
        return g_app.monitors[a].monitor.rect.left < g_app.monitors[b].monitor.rect.left;
    });
    std::vector<std::wstring> ids;
    for (const std::size_t i : order)
    {
        ids.push_back(g_app.monitors[i].id);
    }
    return ids;
}

const std::wstring& OrderWord()
{
    return g_app.readings[IndexOf(settings::kColumnOrder)].word;
}

void Refresh()
{
    const rules::Look look = CurrentLook();
    std::wstring      text = look.text;
    if (g_app.gameRunning)
    {
        text = L"Close the game to save a change: while it runs it would write its own values over this file.\r\n" + text;
    }
    else if (!g_app.savedNote.empty())
    {
        text = g_app.savedNote + L"\r\n" + text;
    }
    if (g_app.namedNotHere > 0 && !g_app.picksChanged)
    {
        text = std::format(L"{} of the monitors saved before not connected now: Save keeps them unless you click a monitor.\r\n", g_app.namedNotHere) + text;
    }
    SetWindowTextW(g_app.status, text.c_str());
    // No pick at all can always be saved: a desktop with no row still has its other choices.
    EnableWindow(g_app.save, (look.ok || !look.picked) && !g_app.gameRunning);
    InvalidateRect(g_app.window, &g_app.picture, FALSE);
}

void PaintPicture(HDC dc)
{
    const int width  = g_app.picture.right - g_app.picture.left;
    const int height = g_app.picture.bottom - g_app.picture.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }
    const HDC     memory = CreateCompatibleDC(dc);
    const HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    const auto    oldBmp = SelectObject(memory, bitmap);
    const RECT    all{0, 0, width, height};
    FillRect(memory, &all, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(memory, TRANSPARENT);
    const auto oldFont = SelectObject(memory, g_app.smallFont);

    const auto        plain   = PlainMonitors();
    const auto        placed  = rules::Fit(plain, width, height, Px(6));
    const rules::Look look    = CurrentLook();
    const bool        spanOn  = g_app.readings[IndexOf(settings::kMultiMonitor)].word == L"on";
    for (std::size_t i = 0; i < placed.size(); ++i)
    {
        const auto  screen  = std::find(look.screens.begin(), look.screens.end(), i);
        const bool  inRow   = screen != look.screens.end();
        const bool  ticked  = g_app.ticked[i] != 0;
        const COLORREF fill = inRow && spanOn ? RGB(0, 90, 170) : ticked ? RGB(150, 60, 60) : RGB(70, 70, 70);
        const HBRUSH   brush = CreateSolidBrush(fill);
        const HPEN     pen   = CreatePen(PS_SOLID, Px(ticked ? 3 : 1), ticked ? RGB(255, 255, 255) : RGB(30, 30, 30));
        const auto     oldBrush = SelectObject(memory, brush);
        const auto     oldPen   = SelectObject(memory, pen);
        const RECT     rect{placed[i].left, placed[i].top, placed[i].right - Px(2), placed[i].bottom - Px(2)};
        Rectangle(memory, rect.left, rect.top, rect.right, rect.bottom);
        SelectObject(memory, oldBrush);
        SelectObject(memory, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);

        const row::Rect& real = plain[i].rect;
        std::wstring     text = std::format(L"{}\n{} x {}", g_app.monitors[i].name, real.Width(), real.Height());
        if (const std::wstring never = rules::NeverJoins(plain[i]); !never.empty())
        {
            text += L"\n" + never;
        }
        else if (inRow && spanOn)
        {
            const auto at = static_cast<std::size_t>(screen - look.screens.begin());
            if (const auto player = rules::PlayerOnScreen(OrderWord(), look.screens.size(), at))
            {
                text += std::format(L"\nPlayer {}", *player);
            }
        }
        SetTextColor(memory, RGB(255, 255, 255));
        RECT inner{rect.left + Px(6), rect.top + Px(6), rect.right - Px(6), rect.bottom - Px(6)};
        DrawTextW(memory, text.c_str(), -1, &inner, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    BitBlt(dc, g_app.picture.left, g_app.picture.top, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldFont);
    SelectObject(memory, oldBmp);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

void ClickPicture(int x, int y)
{
    const int  width  = g_app.picture.right - g_app.picture.left;
    const int  height = g_app.picture.bottom - g_app.picture.top;
    const auto plain  = PlainMonitors();
    const auto placed = rules::Fit(plain, width, height, Px(6));
    const int  localX = x - g_app.picture.left, localY = y - g_app.picture.top;
    for (std::size_t i = 0; i < placed.size(); ++i)
    {
        if (localX >= placed[i].left && localX < placed[i].right && localY >= placed[i].top && localY < placed[i].bottom)
        {
            if (!rules::NeverJoins(plain[i]).empty())
            {
                return;  // it can never join: the picture already says why
            }
            g_app.ticked[i]    = g_app.ticked[i] != 0 ? 0 : 1;
            g_app.picksChanged = true;
            g_app.savedNote.clear();
            Refresh();
            return;
        }
    }
}

void Save()
{
    if (GameIsRunning())
    {
        g_app.gameRunning = true;
        Refresh();
        return;
    }
    auto next = g_app.readings;
    if (g_app.picksChanged)
    {
        next[IndexOf(settings::kMonitors)] = settings::Judge(settings::kMonitors, row::NamesWord(TickedIds()));
    }
    bool written = false;
    if (const unsigned long error = settings::WriteFromTable(g_app.path, next, &written); error != 0)
    {
        g_app.savedNote = std::format(L"Could not write {} (Windows error {}).", g_app.path, error);
    }
    else
    {
        g_app.readings     = next;
        g_app.savedNote    = L"Saved. Start the game from Steam as always.";
        g_app.namedNotHere = g_app.picksChanged ? 0 : g_app.namedNotHere;
    }
    Refresh();
}

void SetChoice(const settings::Option& option, HWND combo)
{
    const auto picked = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (picked >= 0 && static_cast<std::size_t>(picked) < option.words.size())
    {
        g_app.readings[IndexOf(option)] = settings::Judge(option, option.words[static_cast<std::size_t>(picked)]);
        g_app.savedNote.clear();
        Refresh();
    }
}

void FillCombo(HWND combo, const settings::Option& option)
{
    const std::wstring& word = g_app.readings[IndexOf(option)].word;
    for (std::size_t i = 0; i < option.wordLabels.size(); ++i)
    {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.wordLabels[i]));
        if (word == option.words[i])
        {
            SendMessageW(combo, CB_SETCURSEL, i, 0);
        }
    }
}

void MakeFonts()
{
    if (g_app.font != nullptr)
    {
        DeleteObject(g_app.font);
        DeleteObject(g_app.smallFont);
    }
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, g_app.dpi);
    g_app.font = CreateFontIndirectW(&metrics.lfMessageFont);
    metrics.lfMessageFont.lfHeight = metrics.lfMessageFont.lfHeight * 9 / 10;
    g_app.smallFont = CreateFontIndirectW(&metrics.lfMessageFont);
    for (const HWND control : {g_app.multi, g_app.order, g_app.save, g_app.close, g_app.multiLabel, g_app.orderLabel, g_app.status})
    {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.font), TRUE);
    }
}

void Layout()
{
    RECT client{};
    GetClientRect(g_app.window, &client);
    const int margin = Px(12), line = Px(24), label = Px(150), combo = Px(220);
    const int right  = client.right - margin;
    int       y      = margin;
    MoveWindow(g_app.multiLabel, margin, y + Px(3), label, line, TRUE);
    MoveWindow(g_app.multi, margin + label, y, combo, Px(200), TRUE);
    MoveWindow(g_app.orderLabel, margin + label + combo + margin, y + Px(3), label, line, TRUE);
    MoveWindow(g_app.order, margin + 2 * label + combo + margin, y, Px(100), Px(200), TRUE);
    y += line + margin;
    const int statusHeight = Px(62), buttonHeight = Px(28);
    const int bottom       = client.bottom - margin - buttonHeight;
    g_app.picture          = {margin, y, right, bottom - statusHeight - margin};
    MoveWindow(g_app.status, margin, g_app.picture.bottom + margin / 2, right - margin, statusHeight, TRUE);
    MoveWindow(g_app.close, right - Px(90), bottom, Px(90), buttonHeight, TRUE);
    MoveWindow(g_app.save, right - Px(190), bottom, Px(90), buttonHeight, TRUE);
    InvalidateRect(g_app.window, nullptr, TRUE);
}

HWND MakeControl(const wchar_t* type, const wchar_t* text, DWORD style, int id)
{
    return CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_app.window,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wide, LPARAM param)
{
    switch (message)
    {
        case WM_CREATE:
            g_app.window     = window;
            g_app.dpi        = GetDpiForWindow(window);
            g_app.multiLabel = MakeControl(L"STATIC", settings::kMultiMonitor.label, SS_LEFT, 0);
            g_app.multi      = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdMulti);
            g_app.orderLabel = MakeControl(L"STATIC", settings::kColumnOrder.label, SS_LEFT, 0);
            g_app.order      = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdOrder);
            g_app.status     = MakeControl(L"STATIC", L"", SS_LEFT, 0);
            g_app.save       = MakeControl(WC_BUTTONW, L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdSave);
            g_app.close      = MakeControl(WC_BUTTONW, L"Close", WS_TABSTOP, kIdClose);
            FillCombo(g_app.multi, settings::kMultiMonitor);
            FillCombo(g_app.order, settings::kColumnOrder);
            MakeFonts();
            Layout();
            g_app.gameRunning = GameIsRunning();
            SetTimer(window, kGameTimer, 2000, nullptr);
            Refresh();
            return 0;
        case WM_TIMER:
            if (wide == kGameTimer)
            {
                if (const bool running = GameIsRunning(); running != g_app.gameRunning)
                {
                    g_app.gameRunning = running;
                    Refresh();
                }
            }
            return 0;
        case WM_DPICHANGED:
        {
            g_app.dpi        = HIWORD(wide);
            const RECT* size = reinterpret_cast<const RECT*>(param);
            SetWindowPos(window, nullptr, size->left, size->top, size->right - size->left, size->bottom - size->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            MakeFonts();
            Layout();
            return 0;
        }
        case WM_DISPLAYCHANGE:
            ReadMonitors(TickedIds(), false);
            Refresh();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wide))
            {
                case kIdSave:
                    Save();
                    return 0;
                case kIdClose:
                    DestroyWindow(window);
                    return 0;
                case kIdMulti:
                    if (HIWORD(wide) == CBN_SELCHANGE)
                    {
                        SetChoice(settings::kMultiMonitor, g_app.multi);
                    }
                    return 0;
                case kIdOrder:
                    if (HIWORD(wide) == CBN_SELCHANGE)
                    {
                        SetChoice(settings::kColumnOrder, g_app.order);
                    }
                    return 0;
                default:
                    break;
            }
            break;
        case WM_LBUTTONDOWN:
            ClickPicture(static_cast<short>(LOWORD(param)), static_cast<short>(HIWORD(param)));
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wide), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            const HDC   dc = BeginPaint(window, &paint);
            PaintPicture(dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(window, kGameTimer);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wide, param);
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
    return (std::filesystem::path(self).parent_path() / kFileName).wstring();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    // Before any window or monitor read: the rects and DPIs must be the real ones the game sees.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_app.path     = FindFile();
    g_app.readings = settings::ReadFile(g_app.path);
    ReadMonitors({}, true);

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW type{};
    type.cbSize        = sizeof(type);
    type.lpfnWndProc   = WindowProc;
    type.hInstance     = instance;
    type.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW, as the wide call takes it
    type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    type.lpszClassName = L"LocalSplitScreenSetup";
    RegisterClassExW(&type);

    const UINT dpi = GetDpiForSystem();
    const HWND window = CreateWindowExW(0, type.lpszClassName, L"FF7 Remake Split-Screen: Monitors",
                                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                                        MulDiv(860, static_cast<int>(dpi), 96), MulDiv(560, static_cast<int>(dpi), 96), nullptr, nullptr,
                                        instance, nullptr);
    if (window == nullptr)
    {
        return 1;
    }
    ShowWindow(window, show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}
