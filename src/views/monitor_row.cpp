#include "views/monitor_row.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <format>
#include <optional>
#include <vector>

namespace lss::views::monitor_row {

namespace {

/// One monitor keeping a row from growing: which one (`about`, for naming it once) and the sentence that says what
/// to change in Windows. A misaligned pair always names the lower monitor, whichever side the row grew from.
struct Stop
{
    Rect         about;
    int          order;  // in the reason: a misaligned monitor first (one drag fixes it), then a size, then scaling
    std::wstring text;
};

std::wstring At(const Rect& rect)
{
    return std::format(L"({}, {})", rect.left, rect.top);
}

/// Why `candidate` cannot stand beside `base` in a row, or nothing if it can (a top within the tolerance can).
std::optional<Stop> Mismatch(const Monitor& base, const Monitor& candidate)
{
    if (candidate.dpi != 96)
    {
        return Stop{candidate.rect, 2,
                    std::format(L"the monitor at {} is scaled to {} % in Windows (needs 100 %)", At(candidate.rect),
                                candidate.dpi * 100 / 96)};
    }
    if (candidate.rect.Height() != base.rect.Height())
    {
        return Stop{candidate.rect, 1,
                    std::format(L"the monitor at {} is {}x{}, {} px tall where its neighbour is {} (a row needs one height)",
                                At(candidate.rect), candidate.rect.Width(), candidate.rect.Height(), candidate.rect.Height(),
                                base.rect.Height())};
    }
    const auto off = candidate.rect.top - base.rect.top;
    if (off > kAlignTolerance || off < -kAlignTolerance)
    {
        const Rect& lower = off > 0 ? candidate.rect : base.rect;
        const Rect& upper = off > 0 ? base.rect : candidate.rect;
        const auto  px    = lower.top - upper.top;
        return Stop{lower, 0,
                    std::format(L"the monitor at {} sits {} px lower than its neighbour at {} (more than the {} px the "
                                L"mod lines up itself: move it up in Windows display settings)",
                                At(lower), px, At(upper), kAlignTolerance)};
    }
    return std::nullopt;
}

/// The monitor touching `edge` (its right edge there when `leftOf`, else its left edge) with any vertical overlap.
const Monitor* Neighbour(std::span<const Monitor> monitors, const Rect& edge, bool leftOf) noexcept
{
    for (const auto& m : monitors)
    {
        const bool touches = leftOf ? m.rect.right == edge.left : m.rect.left == edge.right;
        const bool overlap = m.rect.top < edge.bottom && m.rect.bottom > edge.top;
        if (touches && overlap)
        {
            return &m;
        }
    }
    return nullptr;
}

struct Member
{
    Rect placed;   // as Windows has it
    Rect aligned;  // shifted to the row's top (the same when it is there already)
};

struct Row
{
    std::vector<Member> members;  // left to right
    std::vector<Stop>   stops;    // the unequal neighbours met at either end, for the reason
};

/// `member` touches a monitor outside the row above or below it: its bottom on that monitor's top, or its top on
/// its bottom, with a horizontal overlap. A move to such a member's top keeps the row on the rest of the desktop.
bool Attached(std::span<const Monitor> monitors, const std::vector<Member>& row, const Rect& member) noexcept
{
    for (const auto& m : monitors)
    {
        const bool inRow = std::any_of(row.begin(), row.end(), [&](const Member& r) { return r.placed == m.rect; });
        const bool overlap = m.rect.left < member.right && m.rect.right > member.left;
        if (!inRow && overlap && (member.bottom == m.rect.top || member.top == m.rect.bottom))
        {
            return true;
        }
    }
    return false;
}

/// The row around `base`: outward to each side until a monitor is missing, of another height or scaled. The row's top is
/// the top of its first monitor attached to the rest of the desktop (the leftmost's if none is); the others are
/// read as if moved there (the span moves them first).
Row RowAround(std::span<const Monitor> monitors, const Monitor& base)
{
    Row row;
    row.members.push_back({base.rect, base.rect});
    for (const bool leftOf : {true, false})
    {
        Rect edge = base.rect;
        for (;;)
        {
            const Monitor* next = Neighbour(monitors, edge, leftOf);
            if (next == nullptr)
            {
                break;
            }
            if (auto mismatch = Mismatch(base, *next))
            {
                row.stops.push_back(std::move(*mismatch));
                break;
            }
            row.members.push_back({next->rect, next->rect});
            edge = next->rect;
        }
    }
    std::sort(row.members.begin(), row.members.end(),
              [](const Member& a, const Member& b) { return a.placed.left < b.placed.left; });
    auto top = row.members.front().placed.top;
    for (const auto& m : row.members)
    {
        if (Attached(monitors, row.members, m.placed))
        {
            top = m.placed.top;
            break;
        }
    }
    for (auto& m : row.members)
    {
        m.aligned = {m.placed.left, top, m.placed.right, top + m.placed.Height()};
    }
    return row;
}

}  // namespace

Choice Choose(std::span<const Monitor> monitors, std::size_t gameMonitor, std::int32_t columns)
{
    Choice choice;
    if (gameMonitor >= monitors.size())
    {
        choice.why = L"the game's monitor is not in the list";
        return choice;
    }
    if (columns < 1)
    {
        choice.why = L"no columns asked for";
        return choice;
    }
    const Monitor& game = monitors[gameMonitor];

    // Every row on the desktop, the game's first; the longest qualifying one wins, the game's own on a tie.
    std::vector<Row>  rows;
    std::vector<Rect> seen;  // each row by its leftmost rect (the anchor: placed and aligned are the same)
    const Row gameRow = game.dpi == 96 ? RowAround(monitors, game) : Row{};
    const auto consider = [&](const Monitor& base) {
        if (base.dpi != 96)
        {
            return;
        }
        Row row = RowAround(monitors, base);
        if (std::find(seen.begin(), seen.end(), row.members.front().placed) != seen.end())
        {
            return;
        }
        seen.push_back(row.members.front().placed);
        rows.push_back(std::move(row));
    };
    if (!gameRow.members.empty())
    {
        consider(game);
    }
    for (const auto& m : monitors)
    {
        consider(m);
    }

    // The game's own row when it qualifies (rows.front() then), else the longest qualifying row.
    const Row* best = nullptr;
    if (!gameRow.members.empty() && static_cast<std::int32_t>(gameRow.members.size()) >= columns)
    {
        best = &rows.front();
    }
    for (const auto& row : rows)
    {
        if (best == nullptr && static_cast<std::int32_t>(row.members.size()) >= columns)
        {
            best = &row;
        }
        else if (best != nullptr && best != &rows.front() && row.members.size() > best->members.size())
        {
            best = &row;
        }
    }
    if (best == nullptr)
    {
        // Every monitor that stopped a row, each named once, the easiest change first: the user reads what to do.
        std::int32_t      longest = 0;
        std::vector<Stop> stops;
        for (const auto& row : rows)
        {
            longest = std::max(longest, static_cast<std::int32_t>(row.members.size()));
            for (const auto& stop : row.stops)
            {
                const auto named = [&](const Stop& s) { return s.about == stop.about; };
                if (std::find_if(stops.begin(), stops.end(), named) == stops.end())
                {
                    stops.push_back(stop);
                }
            }
        }
        std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.order < b.order; });
        std::wstring stopped;
        for (const auto& stop : stops)
        {
            stopped += (stopped.empty() ? L"" : L"; ") + stop.text;
        }
        choice.rowMonitors = longest;
        choice.stopped     = stopped;
        if (game.dpi != 96 && rows.empty())
        {
            choice.why = std::format(L"the game's monitor is scaled to {} % in Windows (needs 100 %) and no other row of "
                                     L"unscaled monitors exists",
                                     game.dpi * 100 / 96);
        }
        else
        {
            choice.why = std::format(L"{} monitor(s) of one height in the longest row, {} needed{}{}", longest, columns,
                                     stopped.empty() ? L"" : L"; ", stopped);
        }
        return choice;
    }

    const auto& members = best->members;
    const auto  inRow   = std::find_if(members.begin(), members.end(), [&](const Member& m) { return m.placed == game.rect; });
    const auto  gameAt  = inRow == members.end() ? 0 : static_cast<std::int32_t>(inRow - members.begin());
    const auto  start   = std::min(gameAt, static_cast<std::int32_t>(members.size()) - columns);
    const auto  first   = static_cast<std::size_t>(start);
    const auto  last    = static_cast<std::size_t>(start + columns - 1);
    choice.ok           = true;
    choice.rowMonitors  = static_cast<std::int32_t>(members.size());
    choice.gamesRow     = inRow != members.end();
    choice.span         = {members[first].aligned.left, members[first].aligned.top, members[last].aligned.right,
                           members[last].aligned.bottom};
    for (std::size_t i = first; i <= last; ++i)
    {
        choice.widths.push_back(members[i].aligned.Width());
        if (members[i].placed.top != members[i].aligned.top)
        {
            choice.moves.push_back({members[i].placed, members[i].aligned.left, members[i].aligned.top});
        }
    }
    return choice;
}

namespace {

bool Overlaps(const Rect& a, const Rect& b) noexcept
{
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

/// They share a stretch of edge: a shared corner does not join two monitors in Windows.
bool Joined(const Rect& a, const Rect& b) noexcept
{
    const bool sideBySide = (a.right == b.left || b.right == a.left) && a.top < b.bottom && b.top < a.bottom;
    const bool stacked    = (a.bottom == b.top || b.bottom == a.top) && a.left < b.right && b.left < a.right;
    return sideBySide || stacked;
}

Rect MovedTo(const Rect& rect, std::int32_t left, std::int32_t top) noexcept
{
    return {left, top, left + rect.Width(), top + rect.Height()};
}

/// The free place touching a placed monitor that is nearest to where `rect` is now. One always exists: right of whichever
/// placed monitor reaches furthest right, nothing can be in the way.
Rect NearestFree(const Rect& rect, const std::vector<Rect>& placed)
{
    Rect          best     = rect;
    std::int64_t  bestDist = -1;
    const auto    consider = [&](std::int32_t left, std::int32_t top) {
        const Rect here = MovedTo(rect, left, top);
        if (std::any_of(placed.begin(), placed.end(), [&](const Rect& other) { return Overlaps(here, other); }))
        {
            return;
        }
        const std::int64_t dx = static_cast<std::int64_t>(left) - rect.left, dy = static_cast<std::int64_t>(top) - rect.top;
        if (const std::int64_t dist = dx * dx + dy * dy; bestDist < 0 || dist < bestDist)
        {
            best     = here;
            bestDist = dist;
        }
    };
    for (const Rect& other : placed)
    {
        consider(other.right, other.top);
        consider(other.left - rect.Width(), other.top);
        consider(other.left, other.bottom);
        consider(other.left, other.top - rect.Height());
    }
    return best;
}

/// Every monitor's place with the row's left end at `left`, `top`, or nothing if the row cannot stand there: it may not
/// cover the primary monitor, and it must reach the primary through touching monitors.
std::optional<std::vector<Rect>> Layout(std::span<const Monitor> monitors, std::span<const std::size_t> picks, std::size_t primary,
                                        std::int32_t left, std::int32_t top)
{
    std::vector<Rect>        places(monitors.size());
    std::vector<std::size_t> settled;  // placed and known to stay
    std::vector<char>        inRow(monitors.size(), 0);
    std::int32_t             x = left;
    for (const std::size_t pick : picks)
    {
        places[pick] = MovedTo(monitors[pick].rect, x, top);
        inRow[pick]  = 1;
        x += monitors[pick].rect.Width();
        settled.push_back(pick);
    }
    if (inRow[primary] != 0 && !(places[primary] == monitors[primary].rect))
    {
        return std::nullopt;
    }
    if (inRow[primary] == 0)
    {
        places[primary] = monitors[primary].rect;
        for (const std::size_t pick : picks)
        {
            if (Overlaps(places[pick], places[primary]))
            {
                return std::nullopt;
            }
        }
        settled.push_back(primary);
    }

    // The others stay where the row leaves room for them.
    for (std::size_t i = 0; i < monitors.size(); ++i)
    {
        if (i == primary || inRow[i] != 0)
        {
            continue;
        }
        const Rect& here = monitors[i].rect;
        if (std::none_of(settled.begin(), settled.end(), [&](std::size_t other) { return Overlaps(here, places[other]); }))
        {
            places[i] = here;
            settled.push_back(i);
        }
    }

    // What the primary reaches, walking touching monitors.
    std::vector<std::size_t> reached{primary};
    for (std::size_t at = 0; at < reached.size(); ++at)
    {
        for (const std::size_t other : settled)
        {
            if (std::find(reached.begin(), reached.end(), other) == reached.end() && Joined(places[reached[at]], places[other]))
            {
                reached.push_back(other);
            }
        }
    }
    for (const std::size_t pick : picks)
    {
        if (std::find(reached.begin(), reached.end(), pick) == reached.end())
        {
            return std::nullopt;
        }
    }

    // The rest go to the nearest free place beside what is connected, each then part of it.
    std::vector<Rect> connected;
    for (const std::size_t i : reached)
    {
        connected.push_back(places[i]);
    }
    for (std::size_t i = 0; i < monitors.size(); ++i)
    {
        if (std::find(reached.begin(), reached.end(), i) == reached.end())
        {
            places[i] = NearestFree(monitors[i].rect, connected);
            connected.push_back(places[i]);
        }
    }
    return places;
}

/// A sentence for the first monitor keeping `picks` from a row by Windows' scaling or by height; empty if none does.
std::wstring WhyNoRow(std::span<const Monitor> monitors, std::span<const std::size_t> picks)
{
    const Monitor& first = monitors[picks.front()];
    for (const std::size_t pick : picks)
    {
        const Monitor& m = monitors[pick];
        if (m.dpi != 96)
        {
            return std::format(L"the monitor at {} is scaled to {} % in Windows (needs 100 %)", At(m.rect), m.dpi * 100 / 96);
        }
        if (m.rect.Height() != first.rect.Height())
        {
            return std::format(L"the monitor at {} is {} px tall where the first picked is {} (a row needs one height)", At(m.rect),
                               m.rect.Height(), first.rect.Height());
        }
    }
    return {};
}

}  // namespace

Choice Arrange(std::span<const Monitor> monitors, std::span<const std::size_t> picks)
{
    Choice choice;
    choice.rowMonitors = static_cast<std::int32_t>(picks.size());
    if (picks.size() < 2)
    {
        choice.why = L"a row needs at least two monitors";
        return choice;
    }
    for (std::size_t i = 0; i < picks.size(); ++i)
    {
        if (picks[i] >= monitors.size() || std::find(picks.begin(), picks.begin() + static_cast<std::ptrdiff_t>(i), picks[i]) !=
                                               picks.begin() + static_cast<std::ptrdiff_t>(i))
        {
            choice.why = L"the picks name a monitor twice or one that is not listed";
            return choice;
        }
    }
    if (std::wstring why = WhyNoRow(monitors, picks); !why.empty())
    {
        choice.why     = why;
        choice.stopped = std::move(why);
        return choice;
    }
    const auto found = std::find_if(monitors.begin(), monitors.end(), [](const Monitor& m) { return m.rect.left == 0 && m.rect.top == 0; });
    if (found == monitors.end())
    {
        choice.why = L"no monitor sits at the desktop's corner, so Windows' primary monitor is unknown";
        return choice;
    }
    const auto    primary = static_cast<std::size_t>(found - monitors.begin());
    const Rect&   anchor  = monitors[primary].rect;
    const auto    height  = monitors[picks.front()].rect.Height();
    std::int32_t  width   = 0;
    for (const std::size_t pick : picks)
    {
        width += monitors[pick].rect.Width();
    }

    // Where the row's left end may go: with the primary in it, only where the primary keeps its corner; else with one
    // member staying where it is, then beside the primary.
    struct Place
    {
        std::int32_t left, top;
    };
    std::vector<Place> places;
    std::int32_t       before = 0;
    const auto         inRow  = std::find(picks.begin(), picks.end(), primary);
    for (auto it = picks.begin(); it != picks.end(); ++it)
    {
        if (inRow != picks.end() && it == inRow)
        {
            places.push_back({-before, 0});
        }
        else if (inRow == picks.end())
        {
            places.push_back({monitors[*it].rect.left - before, monitors[*it].rect.top});
        }
        before += monitors[*it].rect.Width();
    }
    if (inRow == picks.end())
    {
        places.push_back({anchor.left, anchor.bottom});
        places.push_back({anchor.left, anchor.top - height});
        places.push_back({anchor.right, anchor.top});
        places.push_back({anchor.left - width, anchor.top});
    }

    std::optional<std::vector<Rect>> best;
    std::size_t                      bestMoved = monitors.size() + 1;
    std::int64_t                     bestFar   = 0;
    for (const Place& place : places)
    {
        auto layout = Layout(monitors, picks, primary, place.left, place.top);
        if (!layout)
        {
            continue;
        }
        std::size_t  moved = 0;
        std::int64_t far   = 0;
        for (std::size_t i = 0; i < monitors.size(); ++i)
        {
            if (!((*layout)[i] == monitors[i].rect))
            {
                ++moved;
                far += std::abs(static_cast<std::int64_t>((*layout)[i].left) - monitors[i].rect.left) +
                       std::abs(static_cast<std::int64_t>((*layout)[i].top) - monitors[i].rect.top);
            }
        }
        if (moved < bestMoved || (moved == bestMoved && far < bestFar))
        {
            best      = std::move(layout);
            bestMoved = moved;
            bestFar   = far;
        }
    }
    if (!best)
    {
        choice.why = L"no place on the desktop fits the row";
        return choice;
    }
    choice.ok   = true;
    choice.span = {(*best)[picks.front()].left, (*best)[picks.front()].top, (*best)[picks.back()].right, (*best)[picks.back()].bottom};
    choice.gamesRow = true;
    for (const std::size_t pick : picks)
    {
        choice.widths.push_back(monitors[pick].rect.Width());
    }
    for (std::size_t i = 0; i < monitors.size(); ++i)
    {
        if (!((*best)[i] == monitors[i].rect))
        {
            choice.moves.push_back({monitors[i].rect, (*best)[i].left, (*best)[i].top});
        }
    }
    return choice;
}

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
        if (named.found != kNone)
        {
            picks.found.push_back(named.found);
        }
    }
    return picks;
}

}  // namespace lss::views::monitor_row
