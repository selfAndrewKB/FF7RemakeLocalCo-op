#include "views/display_move.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <optional>

namespace lss::views::display_move {

namespace {

using row_journal::Entry;
using row_journal::Record;
using row_journal::Seen;
using row_journal::Verdict;

constexpr int           kReadTries = 10;       // a desktop or a record that cannot be read now may read in a moment
constexpr unsigned long kReadPause = 100;      // ms between those tries
constexpr DWORD         kMostRecordBytes = 65536;  // a record is one short line per monitor

struct Config
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

std::optional<Config> Query(UINT32 flags)
{
    for (int attempt = 0; attempt < 3; ++attempt)  // the counts can change between the two calls
    {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        Config config;
        config.paths.resize(pathCount);
        config.modes.resize(modeCount);
        DISPLAYCONFIG_TOPOLOGY_ID topology{};
        const LONG result = QueryDisplayConfig(flags, &pathCount, config.paths.data(), &modeCount, config.modes.data(),
                                               (flags & QDC_DATABASE_CURRENT) != 0 ? &topology : nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
        {
            continue;
        }
        if (result != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        config.paths.resize(pathCount);
        config.modes.resize(modeCount);
        return config;
    }
    return std::nullopt;
}

std::wstring SourceName(const DISPLAYCONFIG_PATH_INFO& path)
{
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size      = sizeof(name);
    name.header.adapterId = path.sourceInfo.adapterId;
    name.header.id        = path.sourceInfo.id;
    return DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS ? std::wstring(name.viewGdiDeviceName) : std::wstring();
}

DISPLAYCONFIG_SOURCE_MODE* SourceMode(Config& config, const DISPLAYCONFIG_PATH_INFO& path) noexcept
{
    const auto index = path.sourceInfo.modeInfoIdx;
    if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || index >= config.modes.size() ||
        config.modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
    {
        return nullptr;
    }
    return &config.modes[index].sourceMode;
}

/// A cloned output's paths share a source: it is listed once.
std::vector<Seen> Positions(Config& config)
{
    std::vector<Seen> seen;
    for (const auto& path : config.paths)
    {
        const auto*  mode = SourceMode(config, path);
        std::wstring name = SourceName(path);
        const bool   known = std::any_of(seen.begin(), seen.end(), [&](const Seen& s) { return s.device == name; });
        if (mode != nullptr && !name.empty() && !known)
        {
            seen.push_back({std::move(name), mode->position.x, mode->position.y});
        }
    }
    return seen;
}

}  // namespace

std::vector<Seen> Read() noexcept
try
{
    auto config = Query(QDC_ONLY_ACTIVE_PATHS);
    return config ? Positions(*config) : std::vector<Seen>{};
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
    for (const Seen& place : places)
    {
        bool found = false;
        for (const auto& path : config->paths)
        {
            if (SourceName(path) != place.device)
            {
                continue;
            }
            if (auto* mode = SourceMode(*config, path); mode != nullptr)
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
    auto       a    = Positions(*saved);
    auto       b    = Positions(*now);
    if (a.empty() || b.empty())
    {
        return true;
    }
    const auto byDevice = [](const Seen& x, const Seen& y) { return x.device < y.device; };
    std::sort(a.begin(), a.end(), byDevice);
    std::sort(b.begin(), b.end(), byDevice);
    return a == b;
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

std::uint64_t ThisProcessStart() noexcept
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    {
        return 0;
    }
    return static_cast<std::uint64_t>(created.dwHighDateTime) << 32 | created.dwLowDateTime;
}

unsigned long WriteRecord(const std::wstring& path, const Record& record) noexcept
try
{
    const std::wstring text = row_journal::Format(record);
    const std::wstring temp = path + L".new";
    const HANDLE       file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }
    DWORD      written = 0;
    const auto bytes   = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    const bool wrote   = WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
    DWORD      error   = wrote ? 0 : GetLastError();
    CloseHandle(file);
    if (wrote && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = GetLastError();
    }
    if (error != 0 || !wrote)
    {
        DeleteFileW(temp.c_str());
        return error != 0 ? error : static_cast<DWORD>(ERROR_WRITE_FAULT);
    }
    return 0;
}
catch (...)
{
    return ERROR_OUTOFMEMORY;
}

bool RecordExists(const std::wstring& path) noexcept
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void DeleteRecord(const std::wstring& path) noexcept
{
    DeleteFileW(path.c_str());
}

namespace {

/// What the file holds: `read` false when it could not be opened or read, which says nothing about what is in it.
struct RecordFile
{
    bool                  read = false;
    std::optional<Record> record;
};

RecordFile ReadRecord(const std::wstring& path)
{
    RecordFile   found;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return found;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size))
    {
        if (size.QuadPart > kMostRecordBytes)
        {
            found.read = true;  // no record is this long: a damaged one, not a short read of a good one
        }
        else
        {
            std::wstring text(static_cast<std::size_t>(size.QuadPart) / sizeof(wchar_t), L'\0');
            DWORD        got = 0;
            found.read = ReadFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &got, nullptr) && got == text.size() * sizeof(wchar_t);
            if (found.read)
            {
                found.record = row_journal::Parse(text);
            }
        }
    }
    CloseHandle(file);
    return found;
}

/// The owner is running: the same process number and the same start time.
bool OwnerAlive(const Record& record) noexcept
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, record.ownerPid);
    if (process == nullptr)
    {
        return false;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT && GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                         (static_cast<std::uint64_t>(created.dwHighDateTime) << 32 | created.dwLowDateTime) == record.ownerStart;
    CloseHandle(process);
    return running;
}

std::vector<Seen> ReadSettled()
{
    auto now = Read();
    for (int tries = 1; now.empty() && tries < kReadTries; ++tries)
    {
        Sleep(kReadPause);
        now = Read();
    }
    return now;
}

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
                places.push_back({entry.device, entry.fromX, entry.fromY});
            }
            result = Place(places) == ERROR_SUCCESS && WaitForPlaces(places, 2000) ? Healed::PutBack : Healed::Failed;
            if (result == Healed::Failed)
            {
                BackToSaved();
            }
            break;
        }
    }
    DeleteRecord(path);
    return result;
}

}  // namespace

Healed Heal(const std::wstring& path, std::uint32_t game) noexcept
try
{
    if (!RecordExists(path))
    {
        return Healed::NoRecord;
    }
    RecordFile file = ReadRecord(path);
    for (int tries = 1; !file.read && tries < kReadTries; ++tries)
    {
        Sleep(kReadPause);
        file = ReadRecord(path);
    }
    if (!file.read)
    {
        return Healed::Kept;
    }
    const bool alive   = game == 0 && file.record && OwnerAlive(*file.record);
    const auto verdict = row_journal::Judge(file.record, ReadSettled(), game, alive);
    return Act(path, file.record ? std::span<const Entry>(file.record->entries) : std::span<const Entry>(), verdict);
}
catch (...)
{
    return Healed::Kept;
}

Healed PutBack(const std::wstring& path, std::span<const Entry> entries) noexcept
try
{
    return Act(path, entries, row_journal::Decide(entries, ReadSettled()));
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
