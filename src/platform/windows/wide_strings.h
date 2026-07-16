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

/// Converts a UTF-8 string to the UTF-16 the Win32 API expects.
inline std::wstring wideFromUtf8(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

/// Converts a UTF-16 span to the project's UTF-8 convention.
inline std::string utf8FromWide(const wchar_t* wide, int wideLength)
{
    if (wideLength <= 0) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, wideLength, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, wideLength, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

}  // namespace sidescopes
