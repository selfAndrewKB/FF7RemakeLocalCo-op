#include "views/monitor_row.h"

#include <algorithm>
#include <format>
#include <vector>

namespace lss::views::monitor_row {

namespace {

/// Why `candidate` cannot stand beside `base` in a row, or empty if it can.
std::wstring Mismatch(const Monitor& base, const Monitor& candidate)
{
    if (candidate.dpi != 96)
    {
        return std::format(L"scaled to {} % in Windows (needs 100 %)", candidate.dpi * 100 / 96);
    }
    if (candidate.rect.Width() != base.rect.Width() || candidate.rect.Height() != base.rect.Height())
    {
        return std::format(L"{}x{}, not {}x{}", candidate.rect.Width(), candidate.rect.Height(), base.rect.Width(),
                           base.rect.Height());
    }
    if (candidate.rect.top != base.rect.top)
    {
        return std::format(L"its top edge is {} px off its neighbour's", candidate.rect.top - base.rect.top);
    }
    return {};
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
    std::vector<Rect> rects;      // left to right
    std::wstring      stoppedBy;  // the first unequal neighbour met, for the reason
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
            const auto mismatch = Mismatch(base, *next);
            if (!mismatch.empty())
            {
                if (row.stoppedBy.empty())
                {
                    row.stoppedBy = std::format(L"the monitor at ({}, {}) is {}", next->rect.left, next->rect.top, mismatch);
                }
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
        std::int32_t longest = 0;
        std::wstring stopped = gameRow.stoppedBy;  // the game's own neighbour first, else any row's
        for (const auto& row : rows)
        {
            longest = std::max(longest, static_cast<std::int32_t>(row.rects.size()));
            if (stopped.empty())
            {
                stopped = row.stoppedBy;
            }
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
