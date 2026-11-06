#include "views/display_config.h"

#include "platform/text.h"

#include <utility>

namespace lss::views::display_config {

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

std::optional<Target> TargetOf(const DISPLAYCONFIG_PATH_INFO& path)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size      = sizeof(name);
    name.header.adapterId = path.targetInfo.adapterId;
    name.header.id        = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    return Target{name.monitorDevicePath, name.monitorFriendlyDeviceName};
}

bool HasPath(const std::optional<Target>& target) noexcept
{
    return target && !target->path.empty();
}

std::wstring MonitorId(const DISPLAYCONFIG_PATH_INFO& path, const std::optional<Target>& target)
{
    return platform::Lower(HasPath(target) ? target->path : SourceName(path));
}

const DISPLAYCONFIG_SOURCE_MODE* SourceMode(const Config& config, const DISPLAYCONFIG_PATH_INFO& path) noexcept
{
    const auto index = path.sourceInfo.modeInfoIdx;
    if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || index >= config.modes.size() ||
        config.modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
    {
        return nullptr;
    }
    return &config.modes[index].sourceMode;
}

DISPLAYCONFIG_SOURCE_MODE* SourceMode(Config& config, const DISPLAYCONFIG_PATH_INFO& path) noexcept
{
    return const_cast<DISPLAYCONFIG_SOURCE_MODE*>(SourceMode(std::as_const(config), path));
}

}  // namespace lss::views::display_config
