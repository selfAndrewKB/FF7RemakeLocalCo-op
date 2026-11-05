// LocalSplitScreenSetup.exe: a small window, run once, that says which monitors the game spans. It shows the desktop as
// Windows arranges it, lets the player click the monitors of the row in the order wanted, and writes LocalSplitScreen.ini beside
// itself (or at the path given as its one argument). The game is then started from Steam as always: no launcher. Every name, word
// and rule comes from the mod's own table and row code (platform/settings.h, views/monitor_row), so nothing here can drift from
// what the game does. Nothing is saved while the game runs: the mod keeps the file's values in memory and its Co-op page would
// write them back over the app's.
//
// The same exe is the mod's guard, started by the mod with `--guard <handle> <record>` before it moves any monitor
// (views/display_align): no window, one wait on the game, the monitors put back if the record is still there when the game ends,
// and gone. It runs only while the game does. It leaves one line in `<record>.log` saying what it found.

#include "platform/settings_write.h"
#include "setup/setup_rules.h"
#include "views/display_move.h"
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
constexpr int            kIdBack     = 105;
constexpr const wchar_t* kRecordName = L"LocalSplitScreen.desktop";
constexpr int            kGuardTries = 30;    // about a minute of asking, with the waits inside each try
constexpr DWORD          kGuardPause = 1000;
constexpr UINT_PTR       kGameTimer  = 1;

struct App
{
    HWND         window = nullptr, multi = nullptr, order = nullptr, save = nullptr, close = nullptr, back = nullptr;
    HWND         multiLabel = nullptr, orderLabel = nullptr, status = nullptr;
    HFONT        font = nullptr, smallFont = nullptr;
    UINT         dpi = 96;
    RECT         picture{}, strip{};
    std::wstring path;
    std::array<settings::Reading, settings::kOptions.size()> readings;
    std::vector<listing::Listed> monitors;
    std::vector<std::size_t>     picks;   // the monitors of the row, in the order clicked: left to right
    bool                         picksChanged = false;  // the player clicked a monitor: the file's own names stay otherwise
    std::int32_t                 namedNotHere = 0;      // monitors the file names that are not connected now
    std::size_t                  primary = 0;
    bool                         gameRunning = false;
    std::wstring                 savedNote;
    std::wstring                 fileWord;  // the picks as the file has them: monitors not connected now stay named until a click
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
    return rules::LookAt(PlainMonitors(), g_app.picks, g_app.primary);
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

/// The word of the current picks, as the settings file keeps it.
std::wstring PicksWord()
{
    std::vector<std::wstring> words;
    for (const std::size_t pick : g_app.picks)
    {
        words.push_back(row::PickWord(g_app.monitors[pick].id, g_app.monitors[pick].key));
    }
    return row::NamesWord(words);
}

/// The monitors now, with the picks found among them: the player's clicks, else the file's word.
void ReadMonitors()
{
    const std::wstring word = g_app.picksChanged ? PicksWord() : g_app.fileWord;
    g_app.monitors          = listing::List();
    g_app.primary  = 0;
    std::vector<std::wstring> ids, keys;
    for (std::size_t i = 0; i < g_app.monitors.size(); ++i)
    {
        const row::Rect& rect = g_app.monitors[i].monitor.rect;
        if (rect.left == 0 && rect.top == 0)
        {
            g_app.primary = i;
        }
        ids.push_back(g_app.monitors[i].id);
        keys.push_back(g_app.monitors[i].key);
    }
    const row::Picks picks = row::FindPicks(ids, keys, word);
    g_app.picks            = picks.found;
    g_app.namedNotHere     = picks.named - static_cast<std::int32_t>(picks.found.size());
}

std::wstring RecordPath()
{
    return (std::filesystem::path(g_app.path).parent_path() / kRecordName).wstring();
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
        text = std::format(L"{} of the monitors saved before not connected now: the game waits for them, and Save keeps them unless you click a monitor.\r\n", g_app.namedNotHere) + text;
    }
    SetWindowTextW(g_app.status, text.c_str());
    // No pick at all can always be saved: a desktop with no row still has its other choices.
    EnableWindow(g_app.save, (look.ok || !look.picked) && !g_app.gameRunning);
    // Offered only when there is something to put back: a desktop that differs from what Windows has saved, with no game to hold it.
    EnableWindow(g_app.back, !g_app.gameRunning && !lss::views::display_move::IsSavedLayout());
    InvalidateRect(g_app.window, &g_app.picture, FALSE);
    InvalidateRect(g_app.window, &g_app.strip, FALSE);
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
        const auto  pickAt  = std::find(g_app.picks.begin(), g_app.picks.end(), i);
        const bool  ticked  = pickAt != g_app.picks.end();
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
        if (ticked)
        {
            text += std::format(L"\nPick {}", static_cast<std::size_t>(pickAt - g_app.picks.begin()) + 1);
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

/// The row as it will stand while the game holds it: each monitor as wide as it is, left to right, with its player.
void PaintStrip(HDC dc)
{
    const int width = g_app.strip.right - g_app.strip.left, height = g_app.strip.bottom - g_app.strip.top;
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

    const rules::Look look  = CurrentLook();
    const bool        spanOn = g_app.readings[IndexOf(settings::kMultiMonitor)].word == L"on";
    std::int64_t      total  = 0;
    std::int32_t      tallest = 1;
    for (const std::size_t screen : look.screens)
    {
        total += g_app.monitors[screen].monitor.rect.Width();
        tallest = std::max(tallest, g_app.monitors[screen].monitor.rect.Height());
    }
    const int caption = Px(92);
    RECT      label{0, 0, caption, height};
    SetTextColor(memory, GetSysColor(COLOR_BTNTEXT));
    DrawTextW(memory, L"While the game runs:", -1, &label, DT_WORDBREAK | DT_VCENTER | DT_NOPREFIX);
    int x = caption;
    const int room = width - caption;
    for (std::size_t at = 0; at < look.screens.size() && total > 0; ++at)
    {
        const listing::Listed& m = g_app.monitors[look.screens[at]];
        const int     w     = static_cast<int>(room * m.monitor.rect.Width() / total);
        const HBRUSH  brush = CreateSolidBrush(look.ok && spanOn ? RGB(0, 90, 170) : RGB(70, 70, 70));
        // Top-aligned, as the row stands, and never too short for its two lines: who plays on it matters more than its exact shape.
        const RECT    rect{x, 0, x + w - Px(2), std::max(MulDiv(height, m.monitor.rect.Height(), tallest), Px(36))};
        FillRect(memory, &rect, brush);
        DeleteObject(brush);
        std::wstring text = m.name;
        if (const auto player = rules::PlayerOnScreen(OrderWord(), look.screens.size(), at))
        {
            text += std::format(L"\nPlayer {}", *player);
        }
        RECT inner{rect.left + Px(4), rect.top + Px(4), rect.right - Px(4), rect.bottom - Px(2)};
        SetTextColor(memory, RGB(255, 255, 255));
        DrawTextW(memory, text.c_str(), -1, &inner, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
        x += w;
    }
    BitBlt(dc, g_app.strip.left, g_app.strip.top, width, height, memory, 0, 0, SRCCOPY);
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
            if (const auto at = std::find(g_app.picks.begin(), g_app.picks.end(), i); at != g_app.picks.end())
            {
                g_app.picks.erase(at);
            }
            else
            {
                g_app.picks.push_back(i);
            }
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
        next[IndexOf(settings::kMonitors)] = settings::Judge(settings::kMonitors, PicksWord());
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
        g_app.fileWord     = next[IndexOf(settings::kMonitors)].word;
        g_app.picksChanged = false;
    }
    Refresh();
}

/// The button: the record's old places when it has them, else Windows' saved arrangement.
void PutMonitorsBack()
{
    namespace move = lss::views::display_move;
    if (GameIsRunning())
    {
        g_app.gameRunning = true;
        Refresh();
        return;
    }
    const move::Healed healed = move::Heal(RecordPath(), 0);
    if (!move::IsSavedLayout())
    {
        const long result = move::BackToSaved();
        g_app.savedNote   = result == ERROR_SUCCESS ? L"Your monitors are back as Windows has them saved."
                                                    : std::format(L"Windows would not put them back (error {}).", result);
    }
    else
    {
        g_app.savedNote = healed == move::Healed::NoRecord ? L"Your monitors are already as Windows has them saved." : L"Your monitors are back.";
    }
    ReadMonitors();
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
    for (const HWND control : {g_app.multi, g_app.order, g_app.save, g_app.close, g_app.back, g_app.multiLabel, g_app.orderLabel, g_app.status})
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
    const int statusHeight = Px(80), buttonHeight = Px(28), stripHeight = Px(46);
    const int bottom       = client.bottom - margin - buttonHeight;
    const int stripTop     = bottom - statusHeight - margin - stripHeight;
    g_app.picture          = {margin, y, right, stripTop - margin / 2};
    g_app.strip            = {margin, stripTop, right, stripTop + stripHeight};
    MoveWindow(g_app.status, margin, g_app.strip.bottom + margin / 2, right - margin, statusHeight, TRUE);
    MoveWindow(g_app.back, margin, bottom, Px(170), buttonHeight, TRUE);
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
            g_app.back       = MakeControl(WC_BUTTONW, L"Put my monitors back", WS_TABSTOP, kIdBack);
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
                    ReadMonitors();  // the game's end may have put monitors back
                    Refresh();
                }
                else
                {
                    EnableWindow(g_app.back, !g_app.gameRunning && !lss::views::display_move::IsSavedLayout());
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
            ReadMonitors();
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
                case kIdBack:
                    PutMonitorsBack();
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
            PaintStrip(dc);
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

/// The guard: waits for the game, puts the monitors back if that game's record is still there, says so in one line, and goes.
/// The game's handle is inherited, so a process number used again cannot fool it. True when the arguments asked for a guard;
/// anything but what the mod passes changes nothing and is gone at once.
bool RunGuard(int& exitCode)
{
    int        count = 0;
    wchar_t**  args  = CommandLineToArgvW(GetCommandLineW(), &count);
    const bool asked = args != nullptr && count >= 2 && wcscmp(args[1], L"--guard") == 0;
    if (!asked)
    {
        if (args != nullptr)
        {
            LocalFree(args);
        }
        return false;
    }
    exitCode = 2;
    if (count != 4)
    {
        LocalFree(args);
        return true;
    }
    wchar_t*           end    = nullptr;
    const auto         value  = wcstoull(args[2], &end, 16);
    const bool         number = end != args[2] && *end == L'\0';
    const std::wstring record = args[3];
    LocalFree(args);
    const auto  game = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
    const DWORD pid  = number ? GetProcessId(game) : 0;  // zero for anything that is not a process
    namespace fs     = std::filesystem;
    if (pid == 0 || fs::path(record).filename() != kRecordName)
    {
        return true;
    }
    if (WaitForSingleObject(game, INFINITE) != WAIT_OBJECT_0)
    {
        return true;
    }
    namespace move            = lss::views::display_move;
    // A desktop that cannot be read now usually can in a moment, and the guard has nothing else to do.
    move::Healed healed = move::Heal(record, pid);
    for (int tries = 0; healed == move::Healed::Kept && tries < kGuardTries; ++tries)
    {
        Sleep(kGuardPause);
        healed = move::Heal(record, pid);
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::wstring line = std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02} the game ended: {}\r\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                                          now.wSecond, move::Name(healed));
    const std::wstring logPath = record + L".log";
    if (const HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        log != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(log, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(log);
    }
    exitCode = healed == move::Healed::Failed ? 1 : 0;
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    // Before any window or monitor read: the rects and DPIs must be the real ones the game sees.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (int guardExit = 0; RunGuard(guardExit))
    {
        return guardExit;
    }
    g_app.path     = FindFile();
    g_app.readings = settings::ReadFile(g_app.path);
    // A record with no game running: the game died and so did its guard.
    if (!GameIsRunning())
    {
        if (const auto healed = lss::views::display_move::Heal(RecordPath(), 0); healed != lss::views::display_move::Healed::NoRecord)
        {
            g_app.savedNote = std::format(L"From the last game: {}.", lss::views::display_move::Name(healed));
        }
    }
    g_app.fileWord = g_app.readings[IndexOf(settings::kMonitors)].word;
    ReadMonitors();

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
                                        MulDiv(860, static_cast<int>(dpi), 96), MulDiv(640, static_cast<int>(dpi), 96), nullptr, nullptr,
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
