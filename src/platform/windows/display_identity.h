#pragma once

#include <cstdint>

namespace sidescopes {

// One display identity for the whole Windows layer. DXGI outputs and GDI
// monitors are enumerated through unrelated APIs in unrelated orders, but
// both report the same GDI device name ("\\.\DISPLAY3"); its numeric
// suffix is the display id shared between capture targets and the desktop
// services. Zero means the name did not parse and never matches a real
// display.
inline uint32_t DisplayIdFromDeviceName(const wchar_t* device_name) {
    uint32_t id = 0;
    bool seen_digit = false;
    for (const wchar_t* at = device_name; *at != L'\0'; ++at) {
        if (*at >= L'0' && *at <= L'9') {
            id = id * 10 + static_cast<uint32_t>(*at - L'0');
            seen_digit = true;
        } else {
            id = 0;
            seen_digit = false;
        }
    }
    return seen_digit ? id : 0;
}

}  // namespace sidescopes
