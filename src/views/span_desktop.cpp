#include "views/span_desktop.h"

#include "platform/settings.h"
#include "views/game_window.h"
#include "views/monitor_list.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <format>

namespace lss::views::span_desktop {

std::uint64_t Shape() noexcept
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

Desktop Read(HWND game) noexcept
try
{
    Desktop        desktop;
    const auto     listed = monitor_list::List();
    const HMONITOR own    = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
    desktop.list          = monitor_list::Plain(listed);
    desktop.gameAt        = listed.size();
    for (std::size_t i = 0; i < listed.size(); ++i)
    {
        desktop.devices.push_back(listed[i].device);
        desktop.ids.push_back(listed[i].id);
        if (listed[i].handle == own)
        {
            desktop.gameAt = i;
        }
    }
    desktop.picks = monitor_list::PicksIn(listed, platform::settings::Of(platform::settings::kMonitors).word);
    return desktop;
}
catch (...)
{
    return {};
}

monitor_row::Choice Row(const Desktop& desktop, std::int32_t columns)
{
    return monitor_picks::Row(desktop.list, desktop.picks, desktop.gameAt, columns);
}

monitor_row::Choice WidestRow(const Desktop& desktop, std::int32_t most)
{
    return monitor_picks::WidestRow(desktop.list, desktop.picks, desktop.gameAt, most);
}

std::vector<row_journal::Seen> PlacesFor(const monitor_row::Choice& choice, const Desktop& desktop)
{
    std::vector<row_journal::Seen> places;
    for (const auto& move : choice.moves)
    {
        for (std::size_t i = 0; i < desktop.list.size(); ++i)
        {
            if (desktop.list[i].rect == move.from)
            {
                places.push_back({desktop.ids[i], move.toLeft, move.toTop});
            }
        }
    }
    return places;
}

std::wstring Text(const Desktop& desktop)
{
    std::wstring text;
    for (std::size_t i = 0; i < desktop.list.size(); ++i)
    {
        const auto& m = desktop.list[i];
        text += std::format(L"{}{} {} {} dpi{}", i ? L", " : L"", desktop.devices[i], game_window::RectText(m.rect),
                            i == desktop.gameAt ? L"(the game's)" : L"", m.dpi);
    }
    return text;
}

}  // namespace lss::views::span_desktop
