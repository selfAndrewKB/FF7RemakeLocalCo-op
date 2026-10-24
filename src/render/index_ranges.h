#pragma once

// Ranges of scene object indices as a test command writes them: "a b" pairs (from a, up to but not including b),
// separated by commas, e.g. "0 1204, 2408 4817"; "none" (any case) or an empty line is no range. Only ASCII digits
// and spaces, tabs and line ends count. Pure, so the host tests read it (tests/index_ranges_test.cpp).

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lss::render::index_ranges {

inline bool IsDigit(wchar_t c) noexcept
{
    return c >= L'0' && c <= L'9';
}

inline bool IsSpace(wchar_t c) noexcept
{
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

struct Range
{
    std::int32_t from = 0;  // first index
    std::int32_t to   = 0;  // one past the last
};

/// The ranges in `text`, or nothing when a part is not two whole numbers with from <= to (each below 2^24).
inline std::optional<std::vector<Range>> Parse(std::wstring_view text)
{
    std::vector<Range> out;
    auto               trimmed = [](std::wstring_view s) {
        while (!s.empty() && IsSpace(s.front()))
        {
            s.remove_prefix(1);
        }
        while (!s.empty() && IsSpace(s.back()))
        {
            s.remove_suffix(1);
        }
        return s;
    };
    const std::wstring_view all = trimmed(text);
    const auto isNone = [](std::wstring_view s) {
        return s.size() == 4 && (s[0] | 0x20) == L'n' && (s[1] | 0x20) == L'o' && (s[2] | 0x20) == L'n' &&
               (s[3] | 0x20) == L'e';
    };
    if (all.empty() || isNone(all))
    {
        return out;
    }
    std::size_t start = 0;
    while (start <= all.size())
    {
        const std::size_t       comma = all.find(L',', start);
        const std::wstring_view part  = trimmed(all.substr(start, comma == std::wstring_view::npos ? all.npos
                                                                                                  : comma - start));
        std::int64_t numbers[2]{};
        int          count = 0;
        std::size_t  i     = 0;
        while (i < part.size())
        {
            if (IsSpace(part[i]))
            {
                ++i;
                continue;
            }
            if (count == 2 || !IsDigit(part[i]))
            {
                return std::nullopt;
            }
            std::int64_t value = 0;
            while (i < part.size() && IsDigit(part[i]))
            {
                value = value * 10 + (part[i] - L'0');
                if (value >= (std::int64_t{1} << 24))
                {
                    return std::nullopt;
                }
                ++i;
            }
            numbers[count++] = value;
        }
        if (count != 2 || numbers[0] > numbers[1])
        {
            return std::nullopt;
        }
        out.push_back({static_cast<std::int32_t>(numbers[0]), static_cast<std::int32_t>(numbers[1])});
        if (comma == std::wstring_view::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return out;
}

/// How many indices the ranges cover below `limit` (overlaps counted once).
inline std::int64_t Count(std::vector<Range> ranges, std::int32_t limit)
{
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.from < b.from; });
    std::int64_t total = 0;
    std::int32_t end   = 0;
    for (const Range& r : ranges)
    {
        const std::int32_t from = std::max(r.from, end);
        const std::int32_t to   = std::min(r.to, limit);
        if (to > from)
        {
            total += to - from;
            end = to;
        }
    }
    return total;
}

}  // namespace lss::render::index_ranges
