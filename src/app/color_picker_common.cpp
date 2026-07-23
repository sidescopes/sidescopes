#include "app/color_picker_common.h"

#include <cmath>
#include <cstddef>
#include <cstdio>

namespace sidescopes {

ImVec4 pickerSwatchColor(const FloatColor& source)
{
    return ImVec4(source.r / 255.0f, source.g / 255.0f, source.b / 255.0f, 1.0f);
}

void pinHexOf(const PinBoard& pins, std::size_t index, char* buffer)
{
    std::snprintf(buffer, 8, "#%02X%02X%02X", static_cast<int>(std::lround(pins.color(index).r)),
                  static_cast<int>(std::lround(pins.color(index).g)),
                  static_cast<int>(std::lround(pins.color(index).b)));
}

void formatDeltaE(float deltaE, char (&value)[8])
{
    std::snprintf(value, sizeof(value), "%.1f", deltaE);
}

}  // namespace sidescopes
