#pragma once

// The setup app's rules: what the picture of the desktop shows and what a choice of monitors means. The row itself is the
// mod's own rule (views/monitor_row), so the app never promises a row the game would refuse. Pure: tested on the host
// (tests/setup_rules_test.cpp); the window is setup/main.cpp.

#include "views/monitor_row.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lss::setup::rules {

namespace row = views::monitor_row;

/// The most monitors a row takes: one per player.
inline constexpr std::size_t kMostInRow = 3;

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

/// What the current choice gives.
struct Look
{
    bool                     ok = false;       // a row the mod would take, or no pick and a row it would find itself
    bool                     picked = false;   // the player picked monitors; else any row serves
    std::vector<std::size_t> screens;          // the row's monitors, left to right
    std::wstring             text;             // one or two sentences for the status line
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

/// The row the mod would take from the whole desktop with `gameMonitor` holding the game: every column it can have, up to
/// the most a row takes, as the span asks.
inline row::Choice FindRow(std::span<const row::Monitor> monitors, std::size_t gameMonitor)
{
    auto columns = static_cast<std::int32_t>(std::min(monitors.size(), kMostInRow));
    auto choice  = row::Choose(monitors, gameMonitor, std::max(columns, 2));
    while (!choice.ok && columns > 2 && choice.rowMonitors >= 2)
    {
        columns = std::min(columns - 1, choice.rowMonitors);
        choice  = row::Choose(monitors, gameMonitor, columns);
    }
    return choice;
}

/// What the choice gives: `picks` are the monitors clicked, in order, left to right in the row; `gameMonitor` the monitor the
/// game opens on (the primary).
inline Look LookAt(std::span<const row::Monitor> monitors, std::span<const std::size_t> picks, std::size_t gameMonitor)
{
    Look look;
    if (picks.empty())
    {
        const row::Choice choice = FindRow(monitors, gameMonitor);
        look.ok                  = choice.ok;
        if (!choice.ok)
        {
            look.text = L"None picked, and no monitors make a row as Windows has them. Click two or three monitors, in the order you want them.";
            return look;
        }
        look.screens = ScreensOf(monitors, choice);
        look.text    = std::format(L"None picked: the game takes the longest row it finds, these {} monitors ({} x {}). Click monitors, in "
                                   L"order, to choose your own.",
                                   look.screens.size(), choice.span.Width(), choice.span.Height());
        return look;
    }
    look.picked = true;
    if (picks.size() == 1)
    {
        look.text = L"Click one or two more, in the order you want them, left to right. Click a monitor again to take it out.";
        return look;
    }
    if (picks.size() > kMostInRow)
    {
        look.text = std::format(L"A row takes up to {} monitors, one per player.", kMostInRow);
        return look;
    }
    const row::Choice choice = row::Arrange(monitors, picks);
    look.ok                  = choice.ok;
    if (!choice.ok)
    {
        look.text = L"These monitors cannot make a row: " + choice.why + L".";
        return look;
    }
    look.screens.assign(picks.begin(), picks.end());
    look.text = std::format(L"A row of {} monitors: one view on each, fitted to its monitor.", picks.size());
    if (std::any_of(choice.widths.begin(), choice.widths.end(), [](std::int32_t width) { return width > row::kWidestCleanView; }))
    {
        look.text += std::format(L" A view wider than {} px shows a band of wrong colours at its right side.", row::kWidestCleanView);
    }
    if (!choice.moves.empty())
    {
        look.text += L" While the game holds the row your monitors move into this order; they go back afterwards, even after a crash. Windows' own "
                     L"arrangement is never changed.";
    }
    return look;
}

/// The player a screen of the row shows, by the Screen Order word ("213": player 2 on the first screen); a row of two screens
/// skips the player beyond them. None past the screens.
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

}  // namespace lss::setup::rules
