// The setup app's state and what changes it: the settings file's values, the monitors and the picks among them, and what a
// press on a part of the window does. The window that shows it is setup/main.cpp; the rules it follows are setup/setup_rules.h.

#include "setup/app.h"

#include "platform/image.h"
#include "platform/settings_write.h"
#include "setup/tips.h"
#include "views/display_move.h"
#include "views/monitor_picks.h"
#include "views/record_file.h"

#include <TlHelp32.h>

#include <algorithm>
#include <filesystem>
#include <format>

namespace lss::setup::app {

namespace {

constexpr const wchar_t* kRestoreHelp =
    L"Puts your monitors back as Windows has them saved, in case the game failed to do so.";

}  // namespace

App g_app;

int Px(int value)
{
    return MulDiv(value, static_cast<int>(g_app.dpi), 96);
}

std::size_t IndexOf(const settings::Option& option)
{
    return static_cast<std::size_t>(std::find(settings::kOptions.begin(), settings::kOptions.end(), &option) - settings::kOptions.begin());
}

namespace {

std::vector<row::Monitor> PlainMonitors()
{
    return listing::Plain(g_app.monitors);
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

std::vector<row::Rect> TilesIn(const rules::Frame& frame)
{
    return rules::Tiles(PlainMonitors(), frame.picture, Px(10), Px(4));
}

}  // namespace

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
        found = _wcsicmp(entry.szExeFile, platform::kGameExe) == 0;
    }
    CloseHandle(snapshot);
    return found;
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
    return (std::filesystem::path(g_app.path).parent_path() / views::record_file::kName).wstring();
}

rules::Frame FrameNow()
{
    RECT client{};
    GetClientRect(g_app.window, &client);
    return rules::Arrange(client.right, client.bottom, g_app.dpi);
}

void Say(std::wstring note, rules::Tone tone)
{
    g_app.note     = std::move(note);
    g_app.noteTone = tone;
}

/// Each part's hover text at the part's place.
void PlaceTips()
{
    if (!tips::Made())
    {
        return;
    }
    const rules::Frame frame = FrameNow();
    const tips::Tip    all[kTipCount] = {{frame.multi, settings::kMultiMonitor.menuHelp},
                                {frame.order, settings::kColumnOrder.menuHelp},
                                {frame.picture, g_app.pictureHelp.c_str()},
                                {frame.status, g_app.status.detail.c_str()},
                                {g_app.canRestore ? frame.restore : row::Rect{}, kRestoreHelp}};
    tips::Place(all);
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
    g_app.canRestore = !g_app.gameRunning && !displays::IsSavedLayout();
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

namespace {

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
        Say(std::format(L"Unable to save (Windows error {}).", error), rules::Tone::Problem);
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
    displays::Heal(RecordPath(), 0);
    if (!displays::IsSavedLayout())
    {
        if (const long result = displays::BackToSaved(); result == ERROR_SUCCESS)
        {
            Say(L"Monitors restored.", rules::Tone::Good);
        }
        else
        {
            Say(std::format(L"Windows was unable to restore the monitors (error {}).", result), rules::Tone::Problem);
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

}  // namespace

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

namespace {

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

}  // namespace

/// `held` is true for the repeats of a key kept down: they move from part to part, but never press or step an option, so a
/// held Enter saves once.
void OnKey(std::uintptr_t key, bool held)
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

}  // namespace lss::setup::app
