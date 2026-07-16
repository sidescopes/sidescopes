// Unit tests for the pure logic of the Windows platform layer: the
// UTF-8/UTF-16 conversions and the display-id parsing. The Win32-coupled
// parts of the layer - DXGI capture, the GDI+ overlays, WinRT face
// detection, window enumeration - need a real desktop and are exercised by
// running the application, not here. The region geometry, now shared across
// platforms, is tested in test_region_geometry.cpp.

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "platform/windows/display_identity.h"
#include "platform/windows/wide_strings.h"

namespace sidescopes {

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16 conversion
// ---------------------------------------------------------------------------

TEST_CASE("ASCII text round-trips through UTF-16 unchanged")
{
    const std::string ascii = "Display 3";
    const std::wstring wide = wideFromUtf8(ascii);
    CHECK(wide.size() == ascii.size());
    CHECK(utf8FromWide(wide.c_str(), static_cast<int>(wide.size())) == ascii);
}

TEST_CASE("Multibyte UTF-8 round-trips and counts code units, not bytes")
{
    const std::string accented = "caf\xC3\xA9";               // "cafe" with a combining-free acute e
    const std::string japanese = "\xE6\x97\xA5\xE6\x9C\xAC";  // two CJK ideographs

    const std::wstring wideAccented = wideFromUtf8(accented);
    CHECK(wideAccented.size() == 4);  // c, a, f, e-acute
    CHECK(utf8FromWide(wideAccented.c_str(), static_cast<int>(wideAccented.size())) == accented);

    const std::wstring wideJapanese = wideFromUtf8(japanese);
    CHECK(wideJapanese.size() == 2);
    CHECK(utf8FromWide(wideJapanese.c_str(), static_cast<int>(wideJapanese.size())) == japanese);
}

TEST_CASE("Empty strings convert to empty strings")
{
    CHECK(wideFromUtf8("").empty());
    CHECK(utf8FromWide(L"ignored", 0).empty());
}

// ---------------------------------------------------------------------------
// Display id parsing
// ---------------------------------------------------------------------------

TEST_CASE("A display device name parses to its numeric suffix")
{
    CHECK(displayIdFromDeviceName(L"\\\\.\\DISPLAY1") == 1u);
    CHECK(displayIdFromDeviceName(L"\\\\.\\DISPLAY2") == 2u);
    CHECK(displayIdFromDeviceName(L"\\\\.\\DISPLAY10") == 10u);
}

TEST_CASE("Only the trailing run of digits counts")
{
    // Digits interrupted by letters reset; the final run is the id.
    CHECK(displayIdFromDeviceName(L"DISPLAY12ABC34") == 34u);
    CHECK(displayIdFromDeviceName(L"\\\\.\\DISPLAY3X") == 0u);
}

TEST_CASE("A name without a usable suffix yields zero")
{
    CHECK(displayIdFromDeviceName(L"") == 0u);
    CHECK(displayIdFromDeviceName(L"NODIGITS") == 0u);
    // A zero suffix is treated as no match: it never names a real display.
    CHECK(displayIdFromDeviceName(L"\\\\.\\DISPLAY0") == 0u);
}

}  // namespace sidescopes
