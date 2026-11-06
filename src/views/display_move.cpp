#include "views/display_move.h"

#include "views/display_config.h"
#include "views/record_file.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <tuple>
#include <utility>

namespace lss::views::display_move {

namespace {

using display_config::Config;
using display_config::Query;
using display_config::SourceMode;
using display_config::SourceName;
using row_journal::Entry;
using row_journal::Seen;
using row_journal::Verdict;

constexpr int           kReadTries = 10;       // a desktop, a monitor's path or a record that cannot be read now may read in a moment
constexpr unsigned long kReadPause = 100;      // ms between those tries

/// Each source once, under the id of its first path's monitor, as views/monitor_list names it: a cloned output's paths share
/// a source. `everyPath` turns false when a monitor's device path did not read, so its id is its source's name.
std::vector<Seen> Positions(const Config& config, bool& everyPath)
{
    std::vector<Seen>         seen;
    std::vector<std::wstring> sources;
    for (const auto& path : config.paths)
    {
        std::wstring source = SourceName(path);
        if (source.empty() || std::find(sources.begin(), sources.end(), source) != sources.end())
        {
            continue;
        }
        sources.push_back(std::move(source));
        if (const auto* mode = SourceMode(config, path); mode != nullptr)
        {
            const auto target = display_config::TargetOf(path);
            everyPath         = everyPath && display_config::HasPath(target);
            seen.push_back({display_config::MonitorId(path, target), mode->position.x, mode->position.y});
        }
    }
    return seen;
}

std::vector<Seen> ReadOnce(bool& everyPath)
{
    everyPath   = true;
    auto config = Query(QDC_ONLY_ACTIVE_PATHS);
    return config ? Positions(*config, everyPath) : std::vector<Seen>{};
}

/// Each monitor's place by the connector it hangs on (the adapter and its output), in one order. Two arrangements are
/// compared by these: Windows numbers the sources of its saved arrangement apart from the live ones, so a source's name
/// can stand for another monitor there, while a connector is the same in both.
using ConnectorPlace = std::tuple<DWORD, LONG, UINT32, LONG, LONG>;

std::vector<ConnectorPlace> PlacesByConnector(const Config& config)
{
    std::vector<ConnectorPlace> places;
    for (const auto& path : config.paths)
    {
        if (const DISPLAYCONFIG_SOURCE_MODE* mode = SourceMode(config, path); mode != nullptr)
        {
            places.emplace_back(path.targetInfo.adapterId.LowPart, path.targetInfo.adapterId.HighPart, path.targetInfo.id, mode->position.x,
                                mode->position.y);
        }
    }
    std::sort(places.begin(), places.end());
    return places;
}

}  // namespace

std::vector<Seen> Read() noexcept
try
{
    bool everyPath = true;
    return ReadOnce(everyPath);
}
catch (...)
{
    return {};
}

std::vector<Seen> ReadSteady() noexcept
try
{
    bool everyPath = true;
    auto now       = ReadOnce(everyPath);
    for (int tries = 1; (now.empty() || !everyPath) && tries < kReadTries; ++tries)
    {
        Sleep(kReadPause);
        now = ReadOnce(everyPath);
    }
    return now;
}
catch (...)
{
    return {};
}

long Place(std::span<const Seen> places) noexcept
try
{
    auto config = Query(QDC_ONLY_ACTIVE_PATHS);
    if (!config)
    {
        return ERROR_GEN_FAILURE;
    }
    std::vector<std::wstring> ids;  // each path's monitor, read once
    for (const auto& path : config->paths)
    {
        ids.push_back(display_config::MonitorId(path, display_config::TargetOf(path)));
    }
    for (const Seen& place : places)
    {
        bool found = false;
        for (std::size_t i = 0; i < config->paths.size(); ++i)
        {
            if (ids[i] != place.monitor)
            {
                continue;
            }
            if (auto* mode = SourceMode(*config, config->paths[i]); mode != nullptr)
            {
                mode->position.x = place.x;
                mode->position.y = place.y;
                found            = true;
            }
        }
        if (!found)
        {
            return ERROR_NOT_FOUND;
        }
    }
    return SetDisplayConfig(static_cast<UINT32>(config->paths.size()), config->paths.data(), static_cast<UINT32>(config->modes.size()),
                            config->modes.data(), SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG);
}
catch (...)
{
    return ERROR_OUTOFMEMORY;
}

long BackToSaved() noexcept
{
    return SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_USE_DATABASE_CURRENT);
}

bool IsSavedLayout() noexcept
try
{
    auto saved = Query(QDC_DATABASE_CURRENT);
    auto now   = Query(QDC_ONLY_ACTIVE_PATHS);
    if (!saved || !now)
    {
        return true;
    }
    const auto a = PlacesByConnector(*saved);
    const auto b = PlacesByConnector(*now);
    return a.empty() || b.empty() || a == b;
}
catch (...)
{
    return true;
}

bool WaitForPlaces(std::span<const Seen> places, unsigned long timeoutMs) noexcept
try
{
    const ULONGLONG start = GetTickCount64();
    for (;;)
    {
        const auto now = Read();
        const bool all = std::all_of(places.begin(), places.end(), [&](const Seen& place) {
            return std::find(now.begin(), now.end(), place) != now.end();
        });
        if (all)
        {
            return true;
        }
        if (GetTickCount64() - start >= timeoutMs)
        {
            return false;
        }
        Sleep(20);
    }
}
catch (...)
{
    return false;
}

namespace {

/// Acts on a verdict and gives up the record only on a definite one.
Healed Act(const std::wstring& path, std::span<const Entry> entries, Verdict verdict)
{
    Healed result = Healed::AlreadyBack;
    switch (verdict)
    {
        case Verdict::Unknown:
            return Healed::Kept;
        case Verdict::NotOurs:
            return Healed::NotOurs;
        case Verdict::AlreadyBack:
            break;
        case Verdict::Changed:
            result = Healed::Changed;
            break;
        case Verdict::Damaged:
            result = Healed::Damaged;
            if (!IsSavedLayout())
            {
                BackToSaved();
            }
            break;
        case Verdict::PutBack:
        {
            std::vector<Seen> places;
            for (const Entry& entry : entries)
            {
                places.push_back({entry.monitor, entry.fromX, entry.fromY});
            }
            result = Place(places) == ERROR_SUCCESS && WaitForPlaces(places, 2000) ? Healed::PutBack : Healed::Failed;
            if (result == Healed::Failed)
            {
                BackToSaved();
            }
            break;
        }
    }
    record_file::Delete(path);
    return result;
}

}  // namespace

Healed Heal(const std::wstring& path, std::uint32_t game) noexcept
try
{
    if (!record_file::Exists(path))
    {
        return Healed::NoRecord;
    }
    record_file::Found file = record_file::Read(path);
    for (int tries = 1; !file.read && tries < kReadTries; ++tries)
    {
        Sleep(kReadPause);
        file = record_file::Read(path);
    }
    if (!file.read)
    {
        return Healed::Kept;
    }
    const bool alive   = game == 0 && file.record && record_file::OwnerAlive(*file.record);
    const auto verdict = row_journal::Judge(file.record, ReadSteady(), game, alive);
    return Act(path, file.record ? std::span<const Entry>(file.record->entries) : std::span<const Entry>(), verdict);
}
catch (...)
{
    return Healed::Kept;
}

Healed PutBack(const std::wstring& path, std::span<const Entry> entries) noexcept
try
{
    return Act(path, entries, row_journal::Decide(entries, ReadSteady()));
}
catch (...)
{
    return Healed::Kept;
}

const wchar_t* Name(Healed healed) noexcept
{
    switch (healed)
    {
        case Healed::NoRecord: return L"nothing was moved";
        case Healed::AlreadyBack: return L"the monitors were already back";
        case Healed::Changed: return L"the desktop had changed since, so it was left as it is";
        case Healed::PutBack: return L"the monitors were put back";
        case Healed::Failed: return L"the monitors would not go back to their old places, so Windows' saved arrangement was asked for";
        case Healed::Kept: return L"no definite answer yet: the record stays for a later try";
        case Healed::NotOurs: return L"the record is another game's, so it stays";
        case Healed::Damaged: return L"the record did not read, so Windows' saved arrangement was asked for if the desktop differed from it";
    }
    return L"";
}

}  // namespace lss::views::display_move
