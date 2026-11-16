#pragma once

// The setup app's rules: what the picture of the desktop shows and what a choice of monitors means. The row itself is the
// mod's own rule (views/monitor_picks, views/monitor_row), so the app never promises a row the game would refuse. Pure: tested
// on the host (tests/setup_rules_test.cpp); the window is setup/main.cpp.

#include "views/monitor_picks.h"
#include "views/monitor_row.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lss::setup::rules {

namespace row = views::monitor_row;

/// The most monitors a row takes: one per player.
inline constexpr std::size_t kMostInRow = static_cast<std::size_t>(views::monitor_picks::kMostInRow);

/// What happens to the desktop while the game holds a row whose monitors do not already stand in it.
inline constexpr const wchar_t* kMovesNote =
    L"While the game runs, your monitors are arranged in this order and returned to the original layout when done.";

/// `monitors` fitted into a box of `width` x `height` with `margin` all round, keeping the desktop's arrangement and shape.
inline std::vector<row::Rect> Fit(std::span<const row::Monitor> monitors, std::int32_t width, std::int32_t height, std::int32_t margin)
{
    std::vector<row::Rect> placed;
    if (monitors.empty())
    {
        return placed;
    }
    row::Rect all = monitors.front().rect;
    for (const row::Monitor& m : monitors)
    {
        all.left   = std::min(all.left, m.rect.left);
        all.top    = std::min(all.top, m.rect.top);
        all.right  = std::max(all.right, m.rect.right);
        all.bottom = std::max(all.bottom, m.rect.bottom);
    }
    const double roomX = std::max(width - 2 * margin, 1);
    const double roomY = std::max(height - 2 * margin, 1);
    const double scale = std::min(roomX / std::max(all.Width(), 1), roomY / std::max(all.Height(), 1));
    const double fromX = (width - all.Width() * scale) / 2;
    const double fromY = (height - all.Height() * scale) / 2;
    const auto   at    = [](double base, double value) { return static_cast<std::int32_t>(base + value + 0.5); };
    for (const row::Monitor& m : monitors)
    {
        placed.push_back({at(fromX, (m.rect.left - all.left) * scale), at(fromY, (m.rect.top - all.top) * scale),
                          at(fromX, (m.rect.right - all.left) * scale), at(fromY, (m.rect.bottom - all.top) * scale)});
    }
    return placed;
}

/// How a line of the status reads: its colour in the window.
enum class Tone : std::uint8_t
{
    Good,     // done, or a row the game takes
    Plain,    // a next step
    Warning,  // works, with something to know
    Problem,  // does not work as it stands
};

/// What the current choice gives.
struct Look
{
    bool                     ok = false;       // a row the mod would take, or no pick and a row it would find itself
    bool                     picked = false;   // monitors are named, by the player's clicks or by the file; else any row serves
    bool                     waits = false;    // fewer than two of the named monitors are here: a row needs more of them
    std::vector<std::size_t> screens;          // the row's monitors, left to right
    std::wstring             text;             // a few words for the status line; empty for a row with nothing to add
    std::wstring             detail;           // the longer explanation, shown on hover; may be empty
    Tone                     tone = Tone::Plain;
};

/// The monitors a choice's span covers, left to right.
inline std::vector<std::size_t> ScreensOf(std::span<const row::Monitor> monitors, const row::Choice& choice)
{
    std::vector<std::size_t> screens;
    for (std::size_t i = 0; i < monitors.size(); ++i)
    {
        const row::Rect& rect = monitors[i].rect;
        const bool       inX  = rect.left >= choice.span.left && rect.right <= choice.span.right;
        const auto       off  = rect.top - choice.span.top;
        if (inX && off >= -row::kAlignTolerance && off <= row::kAlignTolerance)
        {
            screens.push_back(i);
        }
    }
    std::sort(screens.begin(), screens.end(),
              [&](std::size_t a, std::size_t b) { return monitors[a].rect.left < monitors[b].rect.left; });
    return screens;
}

/// What the choice gives: `picks` are the row's monitors that are connected, in order, left to right in the row; `gameMonitor`
/// the monitor the game opens on (the primary); `notConnected` how many more the settings file names that are not connected.
/// The row is the one the game takes when it is asked for every column (monitor_picks::WidestRow): it never puts another
/// monitor in a named one's place, so a file whose monitors are all away gives no row and the game waits for them in one window.
inline Look LookAt(std::span<const row::Monitor> monitors, std::span<const std::size_t> picks, std::size_t gameMonitor,
                   std::int32_t notConnected = 0)
{
    namespace named = views::monitor_picks;
    const named::Picks all{static_cast<std::int32_t>(picks.size()) + notConnected, notConnected, {picks.begin(), picks.end()}};
    const auto         rowNow = [&] { return named::WidestRow(monitors, all, gameMonitor, named::kMostInRow); };
    Look               look;
    if (picks.empty() && notConnected > 0)
    {
        look.picked = true;
        look.waits  = true;
        look.text   = L"No saved monitor is connected.";
        look.detail = L"Reconnect or pick new ones.";
        look.tone   = Tone::Warning;
        return look;
    }
    if (picks.empty())
    {
        const row::Choice choice = rowNow();
        look.ok                  = choice.ok;
        if (!choice.ok)
        {
            look.text = L"Unable to select monitors automatically. Please choose at least 2 monitors.";
            look.tone = Tone::Warning;
            return look;
        }
        look.screens = ScreensOf(monitors, choice);
        look.text    = std::format(L"No monitors selected. The game will use the {} monitors already arranged side by side.",
                                   look.screens.size());
        look.detail  = L"Click monitors to choose your own.";
        look.tone    = Tone::Good;
        return look;
    }
    look.picked = true;
    // One monitor is no row, in the game as here (monitor_picks::PickedColumns).
    if (picks.size() == 1)
    {
        look.waits  = true;
        look.text   = L"Please choose at least 2 monitors.";
        look.detail = L"Unable to save until at least 2 monitors are selected. Click a selected monitor again to deselect it.";
        return look;
    }
    // Only a click too many comes here, and the app saves no such choice. A settings word that names more, written by hand,
    // reads as its first monitors (monitor_picks::FindPicks).
    if (picks.size() > kMostInRow)
    {
        look.text = std::format(L"A maximum of {} monitors can be selected.", kMostInRow);
        look.tone = Tone::Warning;
        return look;
    }
    const row::Choice choice = rowNow();
    look.ok                  = choice.ok;
    if (!choice.ok)
    {
        // One sentence for the player; the reason is the mod's, written to its log when the game tries this choice.
        look.text = L"Unable to arrange the selected monitors. Please choose different monitors.";
        look.tone = Tone::Problem;
        return look;
    }
    // A row that works is shown by the picture alone: the line speaks only when there is something to know.
    look.screens.assign(picks.begin(), picks.end());
    look.tone = Tone::Good;
    if (std::any_of(choice.widths.begin(), choice.widths.end(), [](std::int32_t width) { return width > row::kWidestCleanView; }))
    {
        look.text = std::format(L"Monitors over {} pixels wide currently show a band of incorrect colors due to a lingering bug.",
                                row::kWidestCleanView);
        look.tone = Tone::Warning;
    }
    if (!choice.moves.empty())
    {
        look.detail = kMovesNote;
    }
    return look;
}

/// What else the window knows beside the choice.
struct Notes
{
    bool             gameRunning = false;   // the game is running: nothing can be saved
    bool             inModFolder = true;    // the settings file sits where the mod reads it
    bool             spanOn = true;         // Multiple Monitors is on
    bool             picksChanged = false;  // the player clicked a monitor: a save writes the clicked monitors over the file's
    std::int32_t     notConnected = 0;      // monitors the file names that are not connected; 0 once the player clicks a monitor
    std::wstring_view note;                 // what the last action did; empty once a choice is changed after it
    Tone             noteTone = Tone::Good;
};

/// Whether Save has something it can write. The file's own monitors, untouched, can always be kept beside a change to another
/// option, connected or not; clicked monitors are written only when they make a row, or when every one was taken out.
inline bool CanSave(const Look& look, const Notes& notes) noexcept
{
    return !notes.gameRunning && (!notes.picksChanged || look.ok || !look.picked);
}

/// The window's one status line.
struct Status
{
    std::wstring text;    // empty when there is nothing to say
    std::wstring detail;  // shown on hover; may be empty
    Tone         tone = Tone::Plain;
};

/// The one thing to say, the most pressing first: what stops a save, an action that failed, a file the game will not read
/// (with what the last action did, so a save is still answered), what the last action did, then the choice. Nothing at all
/// for a row that works.
inline Status StatusLine(const Look& look, const Notes& notes)
{
    if (notes.gameRunning)
    {
        return {L"Close the game to save changes.", {}, Tone::Warning};
    }
    if (!notes.note.empty() && notes.noteTone == Tone::Problem)
    {
        return {std::wstring(notes.note), look.detail, Tone::Problem};
    }
    if (!notes.inModFolder)
    {
        return {(notes.note.empty() ? std::wstring() : std::wstring(notes.note) + L" ") + L"Not in the mod's folder: the game won't read these settings.",
                L"Put this app beside the mod's settings file, in the game's ue4ss\\Mods\\LocalSplitScreen folder.", Tone::Problem};
    }
    if (!notes.note.empty())
    {
        return {std::wstring(notes.note), look.detail, notes.noteTone};
    }
    if (notes.notConnected > 0)
    {
        const std::wstring count = notes.notConnected == 1 ? std::wstring(L"1 saved monitor is not connected")
                                                           : std::format(L"{} saved monitors are not connected", notes.notConnected);
        if (look.ok)
        {
            return {count + L".",
                    L"One or more monitors from the last saved layout are missing. The game will use the remaining monitors until "
                    L"they are reconnected.",
                    Tone::Warning};
        }
        // Too few of them here, or the ones here make no row.
        return {count + L": the game will start in split-screen mode.",
                look.waits ? L"At least 2 of the saved monitors must be connected for this feature to function. Reconnect them or "
                             L"choose new monitors."
                           : L"Unable to arrange the connected monitors. Reconnect the missing monitors or choose new ones.",
                Tone::Warning};
    }
    // Clicked monitors that make no row stop a save whatever the option says: that comes first.
    if (!notes.spanOn && (look.ok || !look.picked))
    {
        return {L"Multiple Monitors is off: the game will start in split-screen mode.", {}, Tone::Plain};
    }
    if (look.text.empty())
    {
        return {};
    }
    return {look.text, look.detail, look.tone};
}

/// The hover text over the desktop's picture: what happens to the monitors of a picked row the game will use. Nothing for
/// any other choice: the status line speaks for those.
inline std::wstring_view PictureHelp(const Look& look, const Notes& notes) noexcept
{
    return look.ok && look.picked && notes.spanOn ? std::wstring_view(look.detail) : std::wstring_view();
}

/// The player a screen of the row shows, by the Screen Order word: its digits are the players, the first screen's first. A row
/// of two screens skips the player beyond them. None past the screens.
inline std::optional<int> PlayerOnScreen(std::wstring_view order, std::size_t screens, std::size_t screen)
{
    std::size_t at = 0;
    for (const wchar_t digit : order)
    {
        const int player = digit - L'0';
        if (player < 1 || static_cast<std::size_t>(player) > screens)
        {
            continue;
        }
        if (at++ == screen)
        {
            return player;
        }
    }
    return std::nullopt;
}

// --- the window's layout -------------------------------------------------------------------------------------------------------
// Sizes are written for a 96 dpi screen and scaled by the window's dpi; every rectangle is in the window's client pixels.

/// The smallest client area the layout is drawn for, at 96 dpi.
inline constexpr std::int32_t kMinWidth  = 720;
inline constexpr std::int32_t kMinHeight = 440;

constexpr std::int32_t Scaled(std::int32_t units, std::uint32_t dpi) noexcept
{
    return static_cast<std::int32_t>(units * static_cast<std::int64_t>(dpi) / 96);
}

/// Where each part of the window stands.
struct Frame
{
    row::Rect multi, order;  // the two options, side by side
    row::Rect caption;       // the line over the desktop's picture
    row::Rect picture;       // the desktop, a tile per monitor
    row::Rect status;        // the one status line
    row::Rect restore, save, close;  // the buttons
};

/// The parts of a client area of `width` x `height`: the options on top, the buttons at the bottom, the picture taking what
/// is left between them.
inline Frame Arrange(std::int32_t width, std::int32_t height, std::uint32_t dpi) noexcept
{
    const auto         px     = [dpi](std::int32_t units) { return Scaled(units, dpi); };
    const std::int32_t left   = px(20);
    const std::int32_t right  = width - px(20);
    const std::int32_t middle = width / 2;
    const std::int32_t button = height - px(18) - px(34);
    Frame              frame;
    frame.multi   = {left, px(18), middle - px(6), px(60)};
    frame.order   = {middle + px(6), px(18), right, px(60)};
    frame.caption = {left, px(74), right, px(94)};
    frame.restore = {left, button, left + px(180), button + px(34)};
    frame.close   = {right - px(104), button, right, button + px(34)};
    frame.save    = {frame.close.left - px(130), button, frame.close.left - px(10), button + px(34)};
    frame.status  = {left, button - px(38), right, button - px(14)};
    frame.picture = {left, frame.caption.bottom + px(4), right, frame.status.top - px(12)};
    return frame;
}

/// An option's cell: its name on the left, then the arrow to the value before, the value, and the arrow to the value after.
struct OptionParts
{
    row::Rect previous, value, next;
};

inline OptionParts PartsOf(const row::Rect& cell, std::uint32_t dpi) noexcept
{
    const std::int32_t arrow = Scaled(34, dpi);
    const std::int32_t pad   = Scaled(6, dpi);
    const std::int32_t value = std::min(Scaled(130, dpi), std::max((cell.Width() - 2 * arrow - 2 * pad) / 2, 0));
    OptionParts        parts;
    parts.next     = {cell.right - pad - arrow, cell.top, cell.right - pad, cell.bottom};
    parts.value    = {parts.next.left - value, cell.top, parts.next.left, cell.bottom};
    parts.previous = {parts.value.left - arrow, cell.top, parts.value.left, cell.bottom};
    return parts;
}

/// The desktop's tiles inside `picture`, one per monitor in the list's order, with `gap` between neighbours.
inline std::vector<row::Rect> Tiles(std::span<const row::Monitor> monitors, const row::Rect& picture, std::int32_t margin, std::int32_t gap)
{
    std::vector<row::Rect> tiles = Fit(monitors, picture.Width(), picture.Height(), margin);
    for (row::Rect& tile : tiles)
    {
        tile.left   += picture.left;
        tile.right  += picture.left - gap;
        tile.top    += picture.top;
        tile.bottom += picture.top - gap;
    }
    return tiles;
}

/// A part of the window that takes a click or the keyboard.
enum class Part : std::uint8_t
{
    None,
    Multi,    // the Multiple Monitors option
    Order,    // the Screen Order option
    Tile,     // a monitor of the desktop's picture
    Restore,
    Save,
    Close,
};

struct Hit
{
    Part        part = Part::None;
    std::size_t tile = 0;  // a Tile: the monitor's index
    int         step = 0;  // an option under the mouse: -1 on the arrow to the value before, +1 on the value and the arrow after

    bool operator==(const Hit&) const = default;
};

constexpr bool Inside(const row::Rect& rect, std::int32_t x, std::int32_t y) noexcept
{
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

/// What stands at (x, y). An option answers on its arrows and its value only, never on its name.
inline Hit HitTest(const Frame& frame, std::span<const row::Rect> tiles, std::uint32_t dpi, std::int32_t x, std::int32_t y) noexcept
{
    for (const auto& [cell, part] : {std::pair{frame.multi, Part::Multi}, std::pair{frame.order, Part::Order}})
    {
        const OptionParts parts = PartsOf(cell, dpi);
        if (Inside(parts.previous, x, y))
        {
            return {part, 0, -1};
        }
        if (Inside(parts.value, x, y) || Inside(parts.next, x, y))
        {
            return {part, 0, 1};
        }
    }
    for (std::size_t i = 0; i < tiles.size(); ++i)
    {
        if (Inside(tiles[i], x, y))
        {
            return {Part::Tile, i, 0};
        }
    }
    for (const auto& [rect, part] : {std::pair{frame.restore, Part::Restore}, std::pair{frame.save, Part::Save}, std::pair{frame.close, Part::Close}})
    {
        if (Inside(rect, x, y))
        {
            return {part, 0, 0};
        }
    }
    return {};
}

/// The word `steps` places on from `word` in an option's list, going round at either end; the first word when `word` is not
/// in the list.
inline const wchar_t* Stepped(std::span<const wchar_t* const> words, std::wstring_view word, int steps) noexcept
{
    if (words.empty())
    {
        return L"";
    }
    const auto count = static_cast<int>(words.size());
    for (int i = 0; i < count; ++i)
    {
        if (word == words[static_cast<std::size_t>(i)])
        {
            return words[static_cast<std::size_t>(((i + steps) % count + count) % count)];
        }
    }
    return words.front();
}

}  // namespace lss::setup::rules
