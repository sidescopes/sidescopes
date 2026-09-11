#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/windows/advanced_color.h"

#include <windows.h>

#include <cwchar>
#include <vector>

#include "core/scrgb.h"

namespace sidescopes {

std::optional<double> sdrWhiteNitsFromLevel(uint32_t level)
{
    // DisplayConfig states the level in thousandths of the scRGB white.
    constexpr double LevelsPerScrgbWhite = 1000.0;
    if (level == 0) {
        return std::nullopt;
    }

    return static_cast<double>(level) / LevelsPerScrgbWhite * ScrgbWhiteNits;
}

ColorTarget findColorTarget(const wchar_t* deviceName)
{
    ColorTarget target;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    UINT32 pathCount = 0;
    LONG result = ERROR_INSUFFICIENT_BUFFER;
    // The topology can grow between sizing and querying. Bound retries so a
    // repeatedly changing desktop still returns control to capture recovery.
    for (int attempt = 0; attempt < 3 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
            return target;
        }
        paths.resize(pathCount);
        modes.resize(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    }
    if (result != ERROR_SUCCESS) {
        return target;
    }
    for (UINT32 index = 0; index < pathCount; ++index) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = paths[index].sourceInfo.adapterId;
        source.header.id = paths[index].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            std::wcscmp(source.viewGdiDeviceName, deviceName) != 0) {
            continue;
        }
        target.adapterIdLow = paths[index].targetInfo.adapterId.LowPart;
        target.adapterIdHigh = paths[index].targetInfo.adapterId.HighPart;
        target.id = paths[index].targetInfo.id;
        target.found = true;
        return target;
    }

    return target;
}

std::optional<double> sdrWhiteNits(const ColorTarget& target)
{
    if (!target.found) {
        return std::nullopt;
    }
    DISPLAYCONFIG_SDR_WHITE_LEVEL level{};
    level.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    level.header.size = sizeof level;
    level.header.adapterId.LowPart = target.adapterIdLow;
    level.header.adapterId.HighPart = target.adapterIdHigh;
    level.header.id = target.id;
    if (DisplayConfigGetDeviceInfo(&level.header) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    return sdrWhiteNitsFromLevel(level.SDRWhiteLevel);
}

}  // namespace sidescopes
