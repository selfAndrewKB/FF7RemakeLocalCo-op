#include "views/monitor_row.h"

#include <algorithm>
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

/// Why `candidate` cannot stand beside `base` in a row, or nothing if it can.
std::optional<Stop> Mismatch(const Monitor& base, const Monitor& candidate)
{
    if (candidate.dpi != 96)
    {
        return Stop{candidate.rect, 2,
                    std::format(L"the monitor at {} is scaled to {} % in Windows (needs 100 %)", At(candidate.rect),
                                candidate.dpi * 100 / 96)};
    }
    if (candidate.rect.Width() != base.rect.Width() || candidate.rect.Height() != base.rect.Height())
    {
        return Stop{candidate.rect, 1,
                    std::format(L"the monitor at {} is {}x{}, not {}x{}", At(candidate.rect), candidate.rect.Width(),
                                candidate.rect.Height(), base.rect.Width(), base.rect.Height())};
    }
    if (candidate.rect.top != base.rect.top)
    {
        const Rect& lower = candidate.rect.top > base.rect.top ? candidate.rect : base.rect;
        const Rect& upper = candidate.rect.top > base.rect.top ? base.rect : candidate.rect;
        const auto  px    = lower.top - upper.top;
        return Stop{lower, 0,
                    std::format(L"the monitor at {} sits {} px lower than its neighbour at {} (move it up {} px in "
                                L"Windows display settings)",
                                At(lower), px, At(upper), px)};
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

struct Row
{
    std::vector<Rect> rects;  // left to right
    std::vector<Stop> stops;  // the unequal neighbours met at either end, for the reason
};

/// The row of equal monitors around `base`: outward to each side until one is missing or unequal.
Row RowAround(std::span<const Monitor> monitors, const Monitor& base)
{
    Row row;
    row.rects.push_back(base.rect);
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
            row.rects.push_back(next->rect);
            edge = next->rect;
        }
    }
    std::sort(row.rects.begin(), row.rects.end(), [](const Rect& a, const Rect& b) { return a.left < b.left; });
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
    std::vector<Row> rows;
    std::vector<Rect> seen;  // each row by its leftmost rect
    const Row gameRow = game.dpi == 96 ? RowAround(monitors, game) : Row{};
    const auto consider = [&](const Monitor& base) {
        if (base.dpi != 96)
        {
            return;
        }
        Row row = RowAround(monitors, base);
        if (std::find(seen.begin(), seen.end(), row.rects.front()) != seen.end())
        {
            return;
        }
        seen.push_back(row.rects.front());
        rows.push_back(std::move(row));
    };
    if (!gameRow.rects.empty())
    {
        consider(game);
    }
    for (const auto& m : monitors)
    {
        consider(m);
    }

    // The game's own row when it qualifies (rows.front() then), else the longest qualifying row.
    const Row* best = nullptr;
    if (!gameRow.rects.empty() && static_cast<std::int32_t>(gameRow.rects.size()) >= columns)
    {
        best = &rows.front();
    }
    for (const auto& row : rows)
    {
        if (best == nullptr && static_cast<std::int32_t>(row.rects.size()) >= columns)
        {
            best = &row;
        }
        else if (best != nullptr && best != &rows.front() && row.rects.size() > best->rects.size())
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
            longest = std::max(longest, static_cast<std::int32_t>(row.rects.size()));
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
        if (game.dpi != 96 && rows.empty())
        {
            choice.why = std::format(L"the game's monitor is scaled to {} % in Windows (needs 100 %) and no other row of "
                                     L"unscaled equal monitors exists",
                                     game.dpi * 100 / 96);
        }
        else
        {
            choice.why = std::format(L"{} equal monitor(s) in the longest row, {} needed{}{}", longest, columns,
                                     stopped.empty() ? L"" : L"; ", stopped);
        }
        return choice;
    }

    const auto& row    = best->rects;
    const auto  inRow  = std::find(row.begin(), row.end(), game.rect);
    const auto  gameAt = inRow == row.end() ? 0 : static_cast<std::int32_t>(inRow - row.begin());
    const auto  start  = std::min(gameAt, static_cast<std::int32_t>(row.size()) - columns);
    choice.ok          = true;
    choice.rowMonitors = static_cast<std::int32_t>(row.size());
    choice.gamesRow    = inRow != row.end();
    choice.span        = {row[static_cast<std::size_t>(start)].left, row.front().top,
                          row[static_cast<std::size_t>(start + columns - 1)].right, row.front().bottom};
    return choice;
}

}  // namespace lss::views::monitor_row
