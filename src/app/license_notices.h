#pragma once

#include <span>
#include <string_view>

namespace sidescopes {

struct LicenseNotice
{
    const char* name;
    std::string_view text;
};

/// Complete distribution notices collected from the linked dependency sources.
/// Both names and texts have static storage; texts are also null-terminated.
[[nodiscard]] std::span<const LicenseNotice> licenseNotices();

}  // namespace sidescopes
