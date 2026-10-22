#include "views/display_align.h"

#include "platform/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <format>
#include <optional>
#include <vector>

namespace lss::views::display_align {

namespace {

struct Shift  // one monitor moved by the span (not `Moved`: that is the query below)
{
    std::wstring device;
    std::int32_t fromTop = 0;
    std::int32_t toTop   = 0;
};

std::vector<Shift> g_moved;

/// The active paths and their modes, as Windows has them now.
struct Config
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

std::optional<Config> Query(LONG* error) noexcept
{
    for (int attempt = 0; attempt < 3; ++attempt)  // the counts can change between the two calls
    {
        UINT32 pathCount = 0, modeCount = 0;
        LONG   result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS)
        {
            *error = result;
            return std::nullopt;
        }
        Config config;
        config.paths.resize(pathCount);
        config.modes.resize(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, config.paths.data(), &modeCount, config.modes.data(),
                                    nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
        {
            continue;
        }
        if (result != ERROR_SUCCESS)
        {
            *error = result;
            return std::nullopt;
        }
        config.paths.resize(pathCount);
        config.modes.resize(modeCount);
        return config;
    }
    *error = ERROR_INSUFFICIENT_BUFFER;
    return std::nullopt;
}

/// The GDI device name of a path's source (`\\.\DISPLAYn`), or empty.
std::wstring SourceName(const DISPLAYCONFIG_PATH_INFO& path) noexcept
{
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size      = sizeof(name);
    name.header.adapterId = path.sourceInfo.adapterId;
    name.header.id        = path.sourceInfo.id;
    return DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS ? std::wstring(name.viewGdiDeviceName) : std::wstring();
}

/// The source mode (size and desktop position) of the device, or null when it is not an active source.
DISPLAYCONFIG_SOURCE_MODE* SourceMode(Config& config, const std::wstring& device) noexcept
{
    for (const auto& path : config.paths)
    {
        if (SourceName(path) != device)
        {
            continue;
        }
        const auto index = path.sourceInfo.modeInfoIdx;
        if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || index >= config.modes.size() ||
            config.modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
        {
            return nullptr;
        }
        return &config.modes[index].sourceMode;
    }
    return nullptr;
}

/// Sets each device's top edge and applies the configuration for the session. `record` (optional) receives what
/// moved from where. `what` names the step in the log.
bool Set(std::span<const Move> moves, std::vector<Shift>* record, const wchar_t* what) noexcept
{
    LONG error  = 0;
    auto config = Query(&error);
    if (!config)
    {
        platform::LogWarning(L"window span: the display configuration could not be read (error {}); the {} is not done",
                             error, what);
        return false;
    }
    std::vector<Shift> planned;
    for (const auto& move : moves)
    {
        auto* mode = SourceMode(*config, move.device);
        if (mode == nullptr)
        {
            platform::LogWarning(L"window span: {} is not an active display source; the {} is not done", move.device, what);
            return false;
        }
        planned.push_back({move.device, mode->position.y, move.toTop});
        mode->position.y = move.toTop;
    }
    const LONG result = SetDisplayConfig(static_cast<UINT32>(config->paths.size()), config->paths.data(),
                                         static_cast<UINT32>(config->modes.size()), config->modes.data(),
                                         SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
    if (result != ERROR_SUCCESS)
    {
        platform::LogWarning(L"window span: Windows refused the {} (SetDisplayConfig error {}); nothing changed", what, result);
        return false;
    }
    for (const auto& moved : planned)
    {
        platform::Log(L"window span: {}: the monitor {} moved from y {} to {} ({} px){}", what, moved.device, moved.fromTop,
                      moved.toTop, moved.toTop - moved.fromTop,
                      record ? L" for this Windows session (put back at the leave)" : L"");
    }
    if (record != nullptr)
    {
        record->insert(record->end(), planned.begin(), planned.end());
    }
    return true;
}

}  // namespace

bool Apply(std::span<const Move> moves) noexcept
{
    if (moves.empty())
    {
        return true;
    }
    std::vector<Shift> record;
    if (!Set(moves, &record, L"line-up"))
    {
        return false;
    }
    g_moved.insert(g_moved.end(), record.begin(), record.end());
    return true;
}

void Restore() noexcept
{
    if (g_moved.empty())
    {
        return;
    }
    std::vector<Move> back;
    for (auto it = g_moved.rbegin(); it != g_moved.rend(); ++it)
    {
        back.push_back({it->device, it->fromTop});
    }
    if (!Set(back, nullptr, L"restore"))
    {
        platform::LogWarning(L"window span: {} moved monitor(s) stay where the span put them; Windows' saved arrangement "
                             L"was never changed and returns at the next sign-in",
                             g_moved.size());
    }
    g_moved.clear();
}

bool Moved() noexcept
{
    return !g_moved.empty();
}

std::wstring Reading() noexcept
{
    std::wstring text;
    for (const auto& moved : g_moved)
    {
        text += std::format(L"{}{} y {} -> {}", text.empty() ? L"" : L", ", moved.device, moved.fromTop, moved.toTop);
    }
    return text;
}

}  // namespace lss::views::display_align
