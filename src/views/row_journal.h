#pragma once

// The record of a desktop the mod has moved, on disk from just before the move until the monitors are back, so whoever finds
// it can undo the move: the guard process when the game dies (setup/main.cpp), the next start of the game or the app when the
// guard died too. Pure rules, tested on the host (tests/row_journal_test.cpp); the file and the display calls are
// views/display_move.
//
// A record is thrown away only on a definite answer. One that cannot be read now, or a desktop that cannot be read now, proves
// nothing, and the record is the only way back to the player's layout. A record also names the game that made it, so nobody
// undoes the row of a game that is still running.
//
// Whoever puts the monitors back first compares them with the record, since Windows or the player may have moved on: any
// monitor coming or going sends Windows back to its saved arrangement by itself, and a layout the player set while the game
// ran is theirs. Only a desktop that still reads as the mod left it, wholly or partly, is changed.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace lss::views::row_journal {

/// A monitor's desktop position by its GDI device name.
struct Seen
{
    std::wstring device;
    std::int32_t x = 0, y = 0;
    bool         operator==(const Seen&) const noexcept = default;
};

/// A monitor of the record: where it was and where the mod put it (the same for one that did not move).
struct Entry
{
    std::wstring device;
    std::int32_t fromX = 0, fromY = 0, toX = 0, toY = 0;
    bool         operator==(const Entry&) const noexcept = default;
};

/// The game that moved the monitors: its process number and start time, which together cannot name another process.
struct Record
{
    std::uint32_t      ownerPid   = 0;
    std::uint64_t      ownerStart = 0;
    std::vector<Entry> entries;
    bool               operator==(const Record&) const noexcept = default;
};

inline std::wstring Format(const Record& record)
{
    std::wstring text = std::format(L"owner\t{}\t{}\n", record.ownerPid, record.ownerStart);
    for (const Entry& e : record.entries)
    {
        text += std::format(L"{}\t{}\t{}\t{}\t{}\n", e.device, e.fromX, e.fromY, e.toX, e.toY);
    }
    return text;
}

namespace detail {

template <class Number>
bool ReadNumber(std::wstring_view text, Number& value) noexcept
{
    if (text.empty() || text.size() > 24)
    {
        return false;
    }
    char   narrow[24]{};
    size_t length = 0;
    for (const wchar_t c : text)
    {
        if ((c < L'0' || c > L'9') && c != L'-')
        {
            return false;
        }
        narrow[length++] = static_cast<char>(c);
    }
    const auto [end, error] = std::from_chars(narrow, narrow + length, value);
    return error == std::errc{} && end == narrow + length;
}

/// The tab-separated fields of a line, up to `parts.size()`; the count found, one more than that when more follow.
inline std::size_t Fields(std::wstring_view line, std::span<std::wstring_view> parts) noexcept
{
    std::size_t count = 0, from = 0;
    while (count < parts.size())
    {
        const std::size_t tab = std::min(line.find(L'\t', from), line.size());
        parts[count++]        = line.substr(from, tab - from);
        if (tab == line.size())
        {
            return count;
        }
        from = tab + 1;
    }
    return count + 1;  // the last field read ended at a tab
}

}  // namespace detail

/// A record, or nothing if any line does not read: a damaged record tells nothing about where the monitors were.
inline std::optional<Record> Parse(std::wstring_view text)
{
    Record record;
    bool   owned = false;
    for (std::size_t at = 0; at < text.size();)
    {
        const std::size_t end  = std::min(text.find(L'\n', at), text.size());
        std::wstring_view line = text.substr(at, end - at);
        at                     = end + 1;
        if (!line.empty() && line.back() == L'\r')
        {
            line.remove_suffix(1);
        }
        if (line.empty())
        {
            continue;
        }
        if (!owned)
        {
            std::wstring_view parts[3];
            if (detail::Fields(line, parts) != 3 || parts[0] != L"owner" || !detail::ReadNumber(parts[1], record.ownerPid) ||
                !detail::ReadNumber(parts[2], record.ownerStart))
            {
                return std::nullopt;
            }
            owned = true;
            continue;
        }
        std::wstring_view parts[5];
        Entry             entry;
        if (detail::Fields(line, parts) != 5 || parts[0].empty() || !detail::ReadNumber(parts[1], entry.fromX) ||
            !detail::ReadNumber(parts[2], entry.fromY) || !detail::ReadNumber(parts[3], entry.toX) || !detail::ReadNumber(parts[4], entry.toY))
        {
            return std::nullopt;
        }
        entry.device = std::wstring(parts[0]);
        record.entries.push_back(std::move(entry));
    }
    return owned ? std::optional<Record>(std::move(record)) : std::nullopt;
}

enum class Verdict
{
    PutBack,      // the desktop reads as the mod left it, or partway there: put it back
    AlreadyBack,  // it reads as it was before the move
    Changed,      // something else has moved it, or a monitor came or went: leave it alone, drop the record
    Unknown,      // the desktop could not be read: keep the record
    NotOurs,      // the record belongs to a game still running, or another game than the one asked about: keep it
    Damaged,      // the record does not read: where the monitors were is lost
};

/// The entries against the desktop now.
inline Verdict Decide(std::span<const Entry> entries, std::span<const Seen> now)
{
    if (entries.empty())
    {
        return Verdict::AlreadyBack;
    }
    if (now.empty())
    {
        return Verdict::Unknown;
    }
    bool allBefore = true;
    for (const Entry& entry : entries)
    {
        const auto seen = std::find_if(now.begin(), now.end(), [&](const Seen& s) { return s.device == entry.device; });
        if (seen == now.end())
        {
            return Verdict::Changed;
        }
        const bool before = seen->x == entry.fromX && seen->y == entry.fromY;
        const bool left   = seen->x == entry.toX && seen->y == entry.toY;
        if (!before && !left)
        {
            return Verdict::Changed;
        }
        allBefore = allBefore && before;
    }
    return allBefore ? Verdict::AlreadyBack : entries.size() == now.size() ? Verdict::PutBack : Verdict::Changed;
}

/// A record as found. `game` is the process number of the game whose end is being handled, 0 when none is (a later start): the
/// record is then anyone's unless `ownerAlive`.
inline Verdict Judge(const std::optional<Record>& record, std::span<const Seen> now, std::uint32_t game, bool ownerAlive)
{
    if (!record)
    {
        return Verdict::Damaged;
    }
    if (game != 0 ? record->ownerPid != game : ownerAlive)
    {
        return Verdict::NotOurs;
    }
    return Decide(record->entries, now);
}

}  // namespace lss::views::row_journal
