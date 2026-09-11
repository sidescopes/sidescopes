#pragma once

#include <cstdint>
#include <optional>

namespace sidescopes {

/// The Windows display target behind a DXGI output, addressed the way the
/// DisplayConfig API addresses it. The SDR white level a desktop composes SDR
/// content at is stated there and nowhere DXGI reaches.
struct ColorTarget
{
    uint32_t adapterIdLow = 0;
    int32_t adapterIdHigh = 0;
    uint32_t id = 0;
    bool found = false;
};

/// Finds the display target whose source is the GDI device @p deviceName
/// (`\\.\DISPLAY1`, as DXGI_OUTPUT_DESC reports it). `found` is false when the
/// display configuration cannot be read or names no such source.
[[nodiscard]] ColorTarget findColorTarget(const wchar_t* deviceName);

/// The SDR white level of @p target in nits, read from the display
/// configuration. An unknown target, failed lookup or zero level has no value;
/// capture must recover rather than normalize with an assumed white level.
[[nodiscard]] std::optional<double> sdrWhiteNits(const ColorTarget& target);

/// Converts the fixed-point SDR white level DisplayConfig reports, where 1000
/// is the scRGB white of 80 nits, to nits. Zero is not a usable white level.
[[nodiscard]] std::optional<double> sdrWhiteNitsFromLevel(uint32_t level);

}  // namespace sidescopes
