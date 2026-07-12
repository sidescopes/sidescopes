#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace sidescopes {

// UTF-8 is the project's string convention; the Win32 API speaks UTF-16.

inline std::wstring WideFromUtf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

inline std::string Utf8FromWide(const wchar_t* wide, int wide_length) {
    if (wide_length <= 0) return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

}  // namespace sidescopes
