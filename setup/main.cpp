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
#include "setup/guard.h"
#include "setup/setup_rules.h"
#include "setup/theme.h"
#include "setup/view.h"
#include "setup_version.h"
#include "views/display_move.h"
#include "views/monitor_list.h"

#include <CommCtrl.h>
#include <TlHelp32.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#define LSS_WIDE_(text) L##text
#define LSS_WIDE(text)  LSS_WIDE_(text)

namespace {

namespace settings = lss::platform::settings;
namespace rules    = lss::setup::rules;
namespace view     = lss::setup::view;
namespace row      = lss::views::monitor_row;
namespace picked   = lss::views::monitor_picks;
namespace listing  = lss::views::monitor_list;
namespace move     = lss::views::display_move;

constexpr const wchar_t* kTitle     = L"Multi Monitor Setup";
constexpr const wchar_t* kFileName  = L"LocalSplitScreen.ini";
constexpr const wchar_t* kModFolder = L"dlls";  // stands beside the settings file where the mod reads it
constexpr const wchar_t* kGameExe   = L"ff7remake_.exe";
constexpr UINT_PTR       kGameTimer = 1;
constexpr int            kWidth     = 880;  // the client area the window opens with, at 96 dpi
constexpr int            kHeight    = 560;

// The parts with a longer explanation on hover.
constexpr UINT_PTR kTipMulti   = 1;
constexpr UINT_PTR kTipOrder   = 2;
constexpr UINT_PTR kTipPicture = 3;
constexpr UINT_PTR kTipStatus  = 4;
constexpr UINT_PTR kTipRestore = 5;
constexpr const wchar_t* kRestoreHelp =
    L"Puts your monitors back as Windows has them saved, in case a game ended without doing so.";

struct App
{
    HWND         window = nullptr;
    HWND         tips   = nullptr;
    view::Fonts  fonts;
    UINT         dpi = 96;
    std::wstring path;
    bool         inModFolder = true;
    std::array<settings::Reading, settings::kOptions.size()> readings;
    std::vector<listing::Listed> monitors;
    std::vector<std::size_t>     picks;   // the monitors of the row, in the order clicked: left to right
    bool                         picksChanged = false;  // the player clicked a monitor: the file's own names stay otherwise
    std::int32_t                 namedNotHere = 0;      // monitors the file names that are not connected now
    std::size_t                  primary = 0;
    std::wstring                 fileWord;  // the picks as the file has them: monitors not connected now stay named until a click
    bool                         gameRunning = false;
    std::wstring                 note;      // what the last action did, until the choice changes
    rules::Tone                  noteTone = rules::Tone::Good;
    // Worked out by Refresh from the above, for the paint, the mouse and the keyboard.
    rules::Look   look;
    rules::Status status;
    std::wstring  pictureHelp;  // the hover text over the desktop's picture
    bool          canSave    = false;
    bool          canRestore = false;
    rules::Hit    hover, pressed, focus;
    bool          showFocus = false;  // the keyboard was used last: the chosen part is marked
    bool          tracking  = false;  // the mouse's leaving is asked for
};
App g_app;

int Px(int value)
{
    return MulDiv(value, static_cast<int>(g_app.dpi), 96);
}

std::vector<row::Monitor> PlainMonitors()
{
    return listing::Plain(g_app.monitors);
}

std::size_t IndexOf(const settings::Option& option)
{
    return static_cast<std::size_t>(std::find(settings::kOptions.begin(), settings::kOptions.end(), &option) - settings::kOptions.begin());
}

const std::wstring& WordOf(const settings::Option& option)
{
    return g_app.readings[IndexOf(option)].word;
}

/// The option's value as the Co-op page names it.
std::wstring LabelOf(const settings::Option& option)
{
    const std::wstring& word = WordOf(option);
    for (std::size_t i = 0; i < option.words.size() && i < option.wordLabels.size(); ++i)
    {
        if (word == option.words[i])
        {
            return option.wordLabels[i];
        }
    }
    return word;
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
        words.push_back(picked::PickWord(g_app.monitors[pick].id, g_app.monitors[pick].key));
    }
    return picked::NamesWord(words);
}

/// Reads the connected monitors and finds the picks among them: the player's clicks, else the file's word.
void ReadMonitors()
{
    const std::wstring word = g_app.picksChanged ? PicksWord() : g_app.fileWord;
    g_app.monitors          = listing::List();
    g_app.primary           = 0;
    for (std::size_t i = 0; i < g_app.monitors.size(); ++i)
    {
        const row::Rect& rect = g_app.monitors[i].monitor.rect;
        if (rect.left == 0 && rect.top == 0)
        {
            g_app.primary = i;
        }
    }
    const picked::Picks picks = listing::PicksIn(g_app.monitors, word);
    g_app.picks               = picks.found;
    g_app.namedNotHere        = picks.away;
}

std::wstring RecordPath()
{
    return (std::filesystem::path(g_app.path).parent_path() / move::kRecordName).wstring();
}

rules::Frame FrameNow()
{
    RECT client{};
    GetClientRect(g_app.window, &client);
    return rules::Arrange(client.right, client.bottom, g_app.dpi);
}

std::vector<row::Rect> TilesIn(const rules::Frame& frame)
{
    return rules::Tiles(PlainMonitors(), frame.picture, Px(10), Px(4));
}

void Say(std::wstring note, rules::Tone tone)
{
    g_app.note     = std::move(note);
    g_app.noteTone = tone;
}

RECT ToRect(const row::Rect& rect)
{
    return {rect.left, rect.top, rect.right, rect.bottom};
}

/// A part's place and words for the hover text. An empty text shows nothing.
void PlaceTip(UINT_PTR id, const row::Rect& at, const std::wstring& text)
{
    TTTOOLINFOW tool{};
    tool.cbSize   = sizeof(tool);
    tool.hwnd     = g_app.window;
    tool.uId      = id;
    tool.rect     = ToRect(at);
    tool.lpszText = const_cast<wchar_t*>(text.c_str());  // copied by the control
    SendMessageW(g_app.tips, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    SendMessageW(g_app.tips, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
}

void PlaceTips()
{
    if (g_app.tips == nullptr)
    {
        return;
    }
    const rules::Frame frame = FrameNow();
    PlaceTip(kTipMulti, frame.multi, settings::kMultiMonitor.menuHelp);
    PlaceTip(kTipOrder, frame.order, settings::kColumnOrder.menuHelp);
    PlaceTip(kTipPicture, frame.picture, g_app.pictureHelp);
    PlaceTip(kTipStatus, frame.status, g_app.status.detail);
    PlaceTip(kTipRestore, g_app.canRestore ? frame.restore : row::Rect{}, kRestoreHelp);
}

/// The hover text in the window's own colours and font, at the window's dpi.
void StyleTips()
{
    if (g_app.tips == nullptr)
    {
        return;
    }
    RECT margin{Px(10), Px(7), Px(10), Px(7)};
    SendMessageW(g_app.tips, TTM_SETTIPBKCOLOR, lss::setup::theme::kPanel.bottom, 0);
    SendMessageW(g_app.tips, TTM_SETTIPTEXTCOLOR, lss::setup::theme::kText, 0);
    SendMessageW(g_app.tips, TTM_SETMARGIN, 0, reinterpret_cast<LPARAM>(&margin));
    SendMessageW(g_app.tips, TTM_SETMAXTIPWIDTH, 0, Px(380));
    SendMessageW(g_app.tips, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fonts.minor), TRUE);
}

void MakeTips()
{
    g_app.tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_app.window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_app.tips == nullptr)
    {
        return;
    }
    SetWindowTheme(g_app.tips, L"", L"");  // the system's look off: the control then takes the colours it is given
    StyleTips();
    SendMessageW(g_app.tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 15000);
    for (const UINT_PTR id : {kTipMulti, kTipOrder, kTipPicture, kTipStatus, kTipRestore})
    {
        wchar_t     none[] = L"";
        TTTOOLINFOW tool{};
        tool.cbSize   = sizeof(tool);
        tool.uFlags   = TTF_SUBCLASS;
        tool.hwnd     = g_app.window;
        tool.uId      = id;
        tool.lpszText = none;
        SendMessageW(g_app.tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

/// The part at a point of the client area; a button that cannot be pressed is no part.
rules::Hit HitAt(int x, int y)
{
    const rules::Frame frame = FrameNow();
    const auto         tiles = TilesIn(frame);
    rules::Hit         hit   = rules::HitTest(frame, tiles, g_app.dpi, x, y);
    if ((hit.part == rules::Part::Save && !g_app.canSave) || (hit.part == rules::Part::Restore && !g_app.canRestore))
    {
        hit = {};
    }
    return hit;
}

/// Works out what the window shows from what changed, and asks for a repaint.
void Refresh()
{
    rules::Notes notes;
    notes.gameRunning  = g_app.gameRunning;
    notes.inModFolder  = g_app.inModFolder;
    notes.spanOn       = WordOf(settings::kMultiMonitor) == L"on";
    notes.picksChanged = g_app.picksChanged;
    notes.notConnected = g_app.picksChanged ? 0 : g_app.namedNotHere;
    g_app.look         = rules::LookAt(PlainMonitors(), g_app.picks, g_app.primary, notes.notConnected);
    notes.note         = g_app.note;
    notes.noteTone     = g_app.noteTone;
    g_app.status       = rules::StatusLine(g_app.look, notes);
    g_app.pictureHelp  = rules::PictureHelp(g_app.look, notes);
    g_app.canSave      = rules::CanSave(g_app.look, notes);
    // Offered only when there is something to put back: a desktop that differs from what Windows has saved, with no game to hold it.
    g_app.canRestore = !g_app.gameRunning && !move::IsSavedLayout();
    if (g_app.focus.part == rules::Part::Tile && g_app.focus.tile >= g_app.monitors.size())
    {
        g_app.focus = {};
    }
    // What stands under the mouse may have changed with the rest: a button that came or went, another monitor's tile.
    if (g_app.tracking)
    {
        POINT at{};
        GetCursorPos(&at);
        ScreenToClient(g_app.window, &at);
        g_app.hover = HitAt(at.x, at.y);
    }
    PlaceTips();
    InvalidateRect(g_app.window, nullptr, FALSE);
}

view::Scene SceneNow()
{
    view::Scene scene;
    scene.frame      = FrameNow();
    scene.multiLabel = settings::kMultiMonitor.label;
    scene.multiValue = LabelOf(settings::kMultiMonitor);
    scene.orderLabel = settings::kColumnOrder.label;
    scene.orderValue = LabelOf(settings::kColumnOrder);
    scene.rowLive    = g_app.look.ok && WordOf(settings::kMultiMonitor) == L"on";
    scene.status     = g_app.status;
    scene.canSave    = g_app.canSave;
    scene.canRestore = g_app.canRestore;
    scene.hover      = g_app.hover;
    scene.pressed    = g_app.pressed;
    scene.focus      = g_app.focus;
    scene.showFocus  = g_app.showFocus;

    const std::wstring&      order   = WordOf(settings::kColumnOrder);
    const auto&              screens = g_app.look.screens;
    const auto               tiles   = TilesIn(scene.frame);
    const bool               noRow   = g_app.look.picked && !g_app.look.ok && g_app.picks.size() != 1;
    for (std::size_t i = 0; i < tiles.size() && i < g_app.monitors.size(); ++i)
    {
        const listing::Listed& monitor = g_app.monitors[i];
        view::Tile             tile;
        tile.rect   = tiles[i];
        tile.name   = monitor.name;
        tile.width  = monitor.monitor.rect.Width();
        tile.height = monitor.monitor.rect.Height();
        if (const auto pick = std::find(g_app.picks.begin(), g_app.picks.end(), i); pick != g_app.picks.end())
        {
            tile.pick    = static_cast<int>(pick - g_app.picks.begin()) + 1;
            tile.problem = noRow;
        }
        if (const auto screen = std::find(screens.begin(), screens.end(), i); screen != screens.end())
        {
            tile.inRow  = true;
            tile.player = rules::PlayerOnScreen(order, screens.size(), static_cast<std::size_t>(screen - screens.begin())).value_or(0);
        }
        scene.tiles.push_back(std::move(tile));
    }
    return scene;
}

void ClickTile(std::size_t tile)
{
    if (tile >= g_app.monitors.size())
    {
        return;
    }
    if (const auto at = std::find(g_app.picks.begin(), g_app.picks.end(), tile); at != g_app.picks.end())
    {
        g_app.picks.erase(at);
    }
    else
    {
        g_app.picks.push_back(tile);
    }
    g_app.picksChanged = true;
    g_app.note.clear();
    Refresh();
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
        Say(std::format(L"Couldn't save (Windows error {}).", error), rules::Tone::Problem);
    }
    else
    {
        g_app.readings     = next;
        g_app.namedNotHere = g_app.picksChanged ? 0 : g_app.namedNotHere;
        g_app.fileWord     = next[IndexOf(settings::kMonitors)].word;
        g_app.picksChanged = false;
        Say(L"Saved.", rules::Tone::Good);
    }
    Refresh();
}

/// The button: the record's old places when it has them, else Windows' saved arrangement.
void PutMonitorsBack()
{
    if (GameIsRunning())
    {
        g_app.gameRunning = true;
        Refresh();
        return;
    }
    move::Heal(RecordPath(), 0);
    if (!move::IsSavedLayout())
    {
        if (const long result = move::BackToSaved(); result == ERROR_SUCCESS)
        {
            Say(L"Monitors restored.", rules::Tone::Good);
        }
        else
        {
            Say(std::format(L"Windows couldn't restore them (error {}).", result), rules::Tone::Problem);
        }
    }
    else
    {
        Say(L"Monitors restored.", rules::Tone::Good);
    }
    ReadMonitors();
    Refresh();
}

/// The option's value moved `steps` places along its list.
void Step(const settings::Option& option, int steps)
{
    g_app.readings[IndexOf(option)] = settings::Judge(option, rules::Stepped(option.words, WordOf(option), steps));
    g_app.note.clear();
    Refresh();
}

/// A press on a part, from the mouse or the keyboard.
void Press(const rules::Hit& hit)
{
    switch (hit.part)
    {
        case rules::Part::Multi:
            Step(settings::kMultiMonitor, hit.step < 0 ? -1 : 1);
            break;
        case rules::Part::Order:
            Step(settings::kColumnOrder, hit.step < 0 ? -1 : 1);
            break;
        case rules::Part::Tile:
            ClickTile(hit.tile);
            break;
        case rules::Part::Restore:
            if (g_app.canRestore)
            {
                PutMonitorsBack();
            }
            break;
        case rules::Part::Save:
            if (g_app.canSave)
            {
                Save();
            }
            break;
        case rules::Part::Close:
            DestroyWindow(g_app.window);
            break;
        case rules::Part::None:
            break;
    }
}

rules::Hit HitAt(LPARAM param)
{
    return HitAt(static_cast<short>(LOWORD(param)), static_cast<short>(HIWORD(param)));
}

/// The parts the keyboard walks, in reading order: the options, the monitors left to right, the buttons.
std::vector<rules::Hit> Stops()
{
    std::vector<rules::Hit> stops{{rules::Part::Multi}, {rules::Part::Order}};
    std::vector<std::size_t> tiles(g_app.monitors.size());
    for (std::size_t i = 0; i < tiles.size(); ++i)
    {
        tiles[i] = i;
    }
    std::sort(tiles.begin(), tiles.end(), [](std::size_t a, std::size_t b) {
        const row::Rect& first  = g_app.monitors[a].monitor.rect;
        const row::Rect& second = g_app.monitors[b].monitor.rect;
        return first.left != second.left ? first.left < second.left : first.top < second.top;
    });
    for (const std::size_t tile : tiles)
    {
        stops.push_back({rules::Part::Tile, tile});
    }
    if (g_app.canRestore)
    {
        stops.push_back({rules::Part::Restore});
    }
    if (g_app.canSave)
    {
        stops.push_back({rules::Part::Save});
    }
    stops.push_back({rules::Part::Close});
    return stops;
}

void MoveFocus(int by)
{
    const auto stops = Stops();
    const auto same  = [](const rules::Hit& stop) { return stop.part == g_app.focus.part && stop.tile == g_app.focus.tile; };
    const auto at    = std::find_if(stops.begin(), stops.end(), same);
    const auto count = static_cast<int>(stops.size());
    const int  next  = at == stops.end() ? (by > 0 ? 0 : count - 1) : ((static_cast<int>(at - stops.begin()) + by) % count + count) % count;
    g_app.focus      = stops[static_cast<std::size_t>(next)];
}

/// `held` is true for the repeats of a key kept down: they move from part to part, but never press or step an option, so a
/// held Enter saves once.
void OnKey(WPARAM key, bool held)
{
    const bool onOption = g_app.showFocus && (g_app.focus.part == rules::Part::Multi || g_app.focus.part == rules::Part::Order);
    if (held && (key == VK_SPACE || key == VK_RETURN || key == VK_ESCAPE))
    {
        return;
    }
    switch (key)
    {
        case VK_ESCAPE:
            DestroyWindow(g_app.window);
            return;
        case VK_TAB:
            MoveFocus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
            break;
        case VK_DOWN:
            MoveFocus(1);
            break;
        case VK_UP:
            MoveFocus(-1);
            break;
        case VK_LEFT:
        case VK_RIGHT:
            if (onOption)
            {
                if (held)
                {
                    return;  // one step for one press: a held key would spin through the values
                }
                Press({g_app.focus.part, 0, key == VK_LEFT ? -1 : 1});
            }
            else
            {
                MoveFocus(key == VK_LEFT ? -1 : 1);
            }
            break;
        case VK_SPACE:
        case VK_RETURN:
            if (g_app.showFocus && g_app.focus.part != rules::Part::None)
            {
                Press({g_app.focus.part, g_app.focus.tile, 1});
            }
            else if (key == VK_RETURN)
            {
                Press({rules::Part::Save});
            }
            break;
        default:
            return;
    }
    if (IsWindow(g_app.window))
    {
        g_app.showFocus = true;
        InvalidateRect(g_app.window, nullptr, FALSE);
    }
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
            MakeTips();
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
                else if (const bool restore = !running && !move::IsSavedLayout(); restore != g_app.canRestore)
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
            StyleTips();  // the hover text takes its new font before the one it holds goes
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
            if (g_app.tips != nullptr)
            {
                DestroyWindow(g_app.tips);  // before the font it holds
                g_app.tips = nullptr;
            }
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
    return (std::filesystem::path(self).parent_path() / kFileName).wstring();
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
        if (const auto healed = move::Heal(RecordPath(), 0); healed != move::Healed::NoRecord)
        {
            Say(std::format(L"From the last game: {}.", move::Name(healed)), healed == move::Healed::Failed ? rules::Tone::Problem : rules::Tone::Good);
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
