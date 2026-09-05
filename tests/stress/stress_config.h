#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace sidescopes::test {

inline uint32_t stressSetting(const char* name, uint32_t fallback, uint32_t maximum)
{
    const char* text = std::getenv(name);
    if (text == nullptr) {
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
