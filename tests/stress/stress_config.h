#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "core/environment.h"

namespace sidescopes::test {

inline uint32_t stressSetting(const char* name, uint32_t fallback, uint32_t maximum)
{
    const std::string text = environmentValue(name);
    if (text.empty()) {
        return fallback;
    }
    const std::string_view value(text);
    uint32_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0 || parsed > maximum) {
        throw std::invalid_argument(name);
    }
    return parsed;
}

}  // namespace sidescopes::test
