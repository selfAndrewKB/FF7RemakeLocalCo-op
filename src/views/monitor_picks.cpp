#include "views/monitor_picks.h"

#include <algorithm>
#include <cwctype>
#include <format>

namespace lss::views::monitor_picks {

std::wstring IdentityKey(std::wstring_view path, std::wstring_view name)
{
    // \\?\display#gsm774b#5&21434701&1&uid4353#{...}: the second part is the model.
    const std::size_t first = path.find(L'#');
    const std::size_t next  = first == std::wstring_view::npos ? first : path.find(L'#', first + 1);
    if (next == std::wstring_view::npos)
    {
        return {};
    }
    std::wstring key(path.substr(first + 1, next - first - 1));
    key += L'/';
    // Monitors pad their names with spaces, and the settings file's reader drops every space and comma: letters and digits only.
    const auto end = name.find_last_not_of(L' ');
    for (std::size_t i = 0; end != std::wstring_view::npos && i <= end; ++i)
    {
        const auto low   = static_cast<wchar_t>(std::towlower(name[i]));
        const bool plain = (low >= L'a' && low <= L'z') || (low >= L'0' && low <= L'9') || low == L'-';
        key += plain ? low : L'_';
    }
    return key;
}

std::wstring PickWord(std::wstring_view id, std::wstring_view key)
{
    std::wstring word(id);
    if (!key.empty())
    {
        word += L'~';
        word += key;
    }
    return word;
}

std::wstring NamesWord(std::span<const std::wstring> pickWords)
{
    std::wstring word;
    for (const std::wstring& pick : pickWords)
    {
        word += (word.empty() ? L"" : L"|") + pick;
    }
    return word;
}

Picks FindPicks(std::span<const std::wstring> ids, std::span<const std::wstring> keys, std::wstring_view word)
{
    struct Named
    {
        std::wstring_view id, key;
        std::size_t       found = static_cast<std::size_t>(-1);
    };
    constexpr auto     kNone = static_cast<std::size_t>(-1);
    std::vector<Named> names;
    for (std::size_t at = 0; at < word.size();)
    {
        const std::size_t end = std::min(word.find(L'|', at), word.size());
        if (end > at)
        {
            const std::wstring_view item  = word.substr(at, end - at);
            const std::size_t       tilde = item.find(L'~');
            Named                   named;
            named.id  = item.substr(0, tilde);
            named.key = tilde == std::wstring_view::npos ? std::wstring_view{} : item.substr(tilde + 1);
            names.push_back(named);
        }
        at = end + 1;
    }
    std::vector<char> taken(ids.size(), 0);
    const auto        take = [&](Named& named, std::size_t i) {
        named.found = i;
        taken[i]    = 1;
    };
    for (Named& named : names)
    {
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            if (taken[i] == 0 && ids[i] == named.id)
            {
                take(named, i);
                break;
            }
        }
    }
    for (Named& named : names)
    {
        for (std::size_t i = 0; named.found == kNone && !named.key.empty() && i < ids.size() && i < keys.size(); ++i)
        {
            if (taken[i] == 0 && keys[i] == named.key)
            {
                take(named, i);
            }
        }
    }
    Picks picks;
    picks.named = static_cast<std::int32_t>(names.size());
    for (const Named& named : names)
    {
        if (named.found == kNone)
        {
            ++picks.away;
        }
        else if (picks.found.size() < static_cast<std::size_t>(kMostInRow))
        {
            picks.found.push_back(named.found);
        }
    }
    return picks;
}

std::int32_t PickedColumns(const Picks& picks) noexcept
{
    const auto found = static_cast<std::int32_t>(picks.found.size());
    return found < 2 ? 0 : std::min(found, kMostInRow);
}

monitor_row::Choice Row(std::span<const monitor_row::Monitor> monitors, const Picks& picks, std::size_t gameMonitor, std::int32_t columns)
{
    if (picks.named == 0)
    {
        return monitor_row::Choose(monitors, gameMonitor, columns);
    }
    const auto& found = picks.found;
    if (found.size() < 2)
    {
        monitor_row::Choice waiting;
        waiting.why = std::format(L"{} of the {} monitors picked in the setup app are connected; the row waits for at least two",
                                  found.size(), picks.named);
        return waiting;
    }
    const auto count = std::min(found.size(), static_cast<std::size_t>(std::max(columns, 0)));
    return monitor_row::Arrange(monitors, std::span(found).first(count));
}

monitor_row::Choice WidestRow(std::span<const monitor_row::Monitor> monitors, const Picks& picks, std::size_t gameMonitor, std::int32_t most)
{
    monitor_row::Choice choice = Row(monitors, picks, gameMonitor, most);
    // Named monitors are taken as they are. A desktop's own longest row may be shorter than what was asked: a two-monitor row
    // spans two columns when three are asked.
    std::int32_t columns = most;
    while (!choice.ok && picks.named == 0 && columns > 2 && choice.rowMonitors >= 2)
    {
        columns = std::min(columns - 1, choice.rowMonitors);
        choice  = monitor_row::Choose(monitors, gameMonitor, columns);
    }
    return choice;
}

}  // namespace lss::views::monitor_picks
